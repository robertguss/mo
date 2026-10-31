module HttpPolicy
expose BodyPlan(..), inspect, body_plan
use Http
use HttpAuthority

enum BodyPlan
  Reject(Int)
  Fixed(length: Int, send_continue: Bool, version: Option(Text))
  Chunked(send_continue: Bool, version: Option(Text))
end

struct Parameter
  name: Http.Span
  value: Http.Span
  quoted: Bool
end

struct Item
  name: Http.Span
  params: List(Parameter)
end

fn byte(bytes: List(Byte), at: Int) : Int
  case bytes.get(at)
    Some(value): value.to_int
    None: -1
  end
end

fn equal?(bytes: List(Byte), part: Http.Span, text: Text) : Bool
  chars = text.chars
  return false if chars.size != part.size
  for at in 0..part.size
    return false if byte(bytes, part.start + at) != (chars.get(at) or ' ').code
  end
  true
end

fn ascii?(bytes: List(Byte), part: Http.Span, text: Text) : Bool
  Http.ascii?(bytes, part, text.chars.map(fn(c) c.code end))
end

fn values(bytes: List(Byte), head: Http.Head, name: Text) : List(Http.Span)
  var found = []
  for field in head.fields
    if ascii?(bytes, field.name, name)
      found = found.append(field.value)
    end
  end
  found
end

fn authority(bytes: List(Byte), part: Http.Span) : Option(HttpAuthority.Authority)
  var chars = []
  for at in part.start..part.start + part.size
    c = byte(bytes, at)
    return None if c < 33 or c > 126
    char = try c.to_char
    chars = chars.append(char)
  end
  HttpAuthority.parse(Text.from_chars(chars))
end

fn inspect(bytes: List(Byte), head: Http.Head, authorities: List(Text)) : Result(
  Option(Http.Span),
  Int
)
  return Error(505) if not equal?(bytes, head.version, "HTTP/1.1")
  hosts = values(bytes, head, "host")
  return Error(400) if hosts.size != 1
  host = case authority(bytes, hosts.get(0) or Http.Span(start: 0, size: 0))
    Some(value): value
    None: return Error(400)
  end
  var effective = host
  var path = head.target
  if ascii?(bytes, Http.Span(start: head.target.start, size: 7), "http://") and
    head.target.size >= 7
    var at = head.target.start + 7
    finish = head.target.start + head.target.size
    start = at
    while at < finish and byte(bytes, at) != 47 and byte(bytes, at) != 63 and byte(bytes, at) != 35
      at += 1
    end
    effective = case authority(bytes, Http.Span(start: start, size: at - start))
      Some(value): value
      None: return Error(400)
    end
    path = Http.Span(start: at, size: finish - at)
  end
  var allowed = false
  for entry in authorities
    case HttpAuthority.parse(entry)
      Some(value):
        if HttpAuthority.equal?(effective, value)
          allowed = true
        end
      None: return Error(400)
    end
  end
  return Error(421) if not allowed
  return Error(403) if not values(bytes, head, "origin").empty?
  return Error(404) if not equal?(bytes, path, "/mcp")
  return Error(405) if not equal?(bytes, head.method, "POST")
  Ok(Http.bearer_span(bytes, head))
end

fn space(bytes: List(Byte), start: Int, finish: Int) : Int
  var at = start
  while at < finish and (byte(bytes, at) == 32 or byte(bytes, at) == 9)
    at += 1
  end
  at
end

fn items(bytes: List(Byte), parts: List(Http.Span)) : Option(List(Item))
  var result = []
  for part in parts
    finish = part.start + part.size
    var at = part.start
    while true
      at = space(bytes, at, finish)
      start = at
      while at < finish and (Http.token?(byte(bytes, at)) or byte(bytes, at) == 47)
        at += 1
      end
      return None if at == start
      name = Http.Span(start: start, size: at - start)
      var params = []
      at = space(bytes, at, finish)
      while at < finish and byte(bytes, at) == 59
        at = space(bytes, at + 1, finish)
        param_start = at
        while at < finish and Http.token?(byte(bytes, at))
          at += 1
        end
        return None if at == param_start
        param_name = Http.Span(start: param_start, size: at - param_start)
        at = space(bytes, at, finish)
        return None if at >= finish or byte(bytes, at) != 61
        at = space(bytes, at + 1, finish)
        quoted = byte(bytes, at) == 34
        if quoted
          at += 1
        end
        value_start = at
        if quoted
          while at < finish and byte(bytes, at) != 34
            if byte(bytes, at) == 92
              at += 1
              return None if at >= finish
            end
            at += 1
          end
          return None if at >= finish
        else
          while at < finish and Http.token?(byte(bytes, at))
            at += 1
          end
          return None if at == value_start
        end
        params = params.append(
          Parameter(
            name: param_name,
            value: Http.Span(start: value_start, size: at - value_start),
            quoted: quoted
          )
        )
        if quoted
          at += 1
        end
        at = space(bytes, at, finish)
      end
      result = result.append(Item(name: name, params: params))
      break if at == finish
      return None if byte(bytes, at) != 44
      at += 1
    end
  end
  Some(result)
end

fn token?(bytes: List(Byte), part: Http.Span) : Bool
  for at in part.start..part.start + part.size
    return false if not Http.token?(byte(bytes, at))
  end
  part.size > 0
end

fn quality(bytes: List(Byte), part: Http.Span) : Option(Int)
  first = byte(bytes, part.start)
  return None if part.size < 1 or part.size > 5 or (first != 48 and first != 49)
  return Some((first - 48) * 1000) if part.size == 1
  return None if byte(bytes, part.start + 1) != 46
  var q = 0
  var scale = 100
  for at in part.start + 2..part.start + part.size
    c = byte(bytes, at)
    return None if c < 48 or c > 57 or (first == 49 and c != 48)
    q += (c - 48) * scale
    scale = scale / 10
  end
  Some(if first == 49: 1000 else: q)
end

fn accepts_json?(bytes: List(Byte), parts: List(Http.Span)) : Bool
  return true if parts.empty?
  parsed = case items(bytes, parts)
    Some(value): value
    None: return false
  end
  var specificity = -1
  var best = 0
  for item in parsed
    rank = if ascii?(bytes, item.name, "application/json")
      2
    else if ascii?(bytes, item.name, "application/*")
      1
    else if ascii?(bytes, item.name, "*/*")
      0
    else
      -1
    end
    var q = 1000
    var seen_q = false
    var parameters = 0
    for param in item.params
      if ascii?(bytes, param.name, "q")
        return false if seen_q or param.quoted
        q = case quality(bytes, param.value)
          Some(value): value
          None: return false
        end
        seen_q = true
      else if not seen_q
        parameters += 1
      end
    end
    if rank >= 0 and parameters == 0 and (rank > specificity or (rank == specificity and q > best))
      specificity = rank
      best = q
    end
  end
  best > 0
end

fn protocol_version(bytes: List(Byte), head: Http.Head) : Option(Text)
  versions = values(bytes, head, "mcp-protocol-version")
  return None if versions.empty?
  if versions.size == 1 and
    equal?(bytes, versions.get(0) or Http.Span(start: 0, size: 0), "2025-11-25")
    Some("2025-11-25")
  else
    Some("")
  end
end

fn length(bytes: List(Byte), part: Http.Span) : Option(Int)
  return None if part.size == 0
  var value = 0
  for at in part.start..part.start + part.size
    c = byte(bytes, at)
    return None if c < 48 or c > 57
    digit = c - 48
    return None if value > 922337203685477580 or (value == 922337203685477580 and digit > 7)
    value = value * 10 + digit
  end
  Some(value)
end

fn body_plan(bytes: List(Byte), head: Http.Head, body_limit: Int) : BodyPlan
  expects = case items(bytes, values(bytes, head, "expect"))
    Some(value): value
    None: return Reject(417)
  end
  for item in expects
    return Reject(417) if not ascii?(bytes, item.name, "100-continue") or not item.params.empty?
  end
  types = case items(bytes, values(bytes, head, "content-type"))
    Some(value): value
    None: return Reject(415)
  end
  return Reject(415) if types.size != 1
  for item in types
    return Reject(415) if not ascii?(bytes, item.name, "application/json")
  end
  return Reject(406) if not accepts_json?(bytes, values(bytes, head, "accept"))
  encodings = case items(bytes, values(bytes, head, "content-encoding"))
    Some(value): value
    None: return Reject(415)
  end
  for item in encodings
    return Reject(415) if not ascii?(bytes, item.name, "identity") or not item.params.empty?
  end
  lengths = values(bytes, head, "content-length")
  transfers = values(bytes, head, "transfer-encoding")
  return Reject(400) if lengths.size > 1 or (not lengths.empty? and not transfers.empty?)
  version = protocol_version(bytes, head)
  if not transfers.empty?
    codings = case items(bytes, transfers)
      Some(value): value
      None: return Reject(400)
    end
    var chunks = 0
    for item in codings
      return Reject(400) if not token?(bytes, item.name)
      if ascii?(bytes, item.name, "chunked")
        chunks += 1
        return Reject(400) if not item.params.empty?
      end
    end
    last = case codings.last
      Some(value): value
      None: return Reject(400)
    end
    return Reject(400) if chunks != 1 or not ascii?(bytes, last.name, "chunked")
    return Reject(501) if codings.size != 1
    return Chunked(send_continue: not expects.empty?, version: version)
  end
  var size = 0
  for part in lengths
    size = case length(bytes, part)
      Some(value): value
      None: return Reject(400)
    end
  end
  return Reject(413) if size > body_limit
  Fixed(length: size, send_continue: not expects.empty?, version: version)
end

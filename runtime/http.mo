module Http
expose Span(..),
Field(..),
Head(..),
HeaderLimits(..),
HeaderScan(..),
HeaderProblem(..),
scan_headers,
parse_headers,
bearer_span,
chunk_size

struct Span
  start: Int
  size: Int
end

struct Field
  name: Span
  value: Span
end

struct Head
  method: Span
  target: Span
  version: Span
  fields: List(Field)
  after: Int
end

struct HeaderLimits
  bytes: Int
  line: Int
  fields: Int
end

struct HeaderScan
  matched: Int
  size: Int
  complete: Bool
end

enum HeaderProblem
  Incomplete
  Malformed
  TooLarge
end

fn scan_headers(bytes: List(Byte), state: HeaderScan, limit: Int) : Result(
  HeaderScan,
  HeaderProblem
)
  return Ok(state) if state.complete
  var matched = state.matched
  var size = state.size
  for byte in bytes
    return Error(TooLarge) if size >= limit
    size += 1
    c = byte.to_int
    if matched == 0 or matched == 2
      if c == 13
        matched += 1
      else
        matched = 0
      end
    else if c == 10
      matched += 1
    else if c == 13
      matched = 1
    else
      matched = 0
    end
    return Ok(HeaderScan(matched: 4, size: size, complete: true)) if matched == 4
  end
  Ok(HeaderScan(matched: matched, size: size, complete: false))
end

fn octet(bytes: List(Byte), at: Int) : Result(Int, HeaderProblem)
  case bytes.get(at)
    Some(byte): Ok(byte.to_int)
    None: Error(Incomplete)
  end
end

fn token?(c: Int) : Bool
  (c >= 48 and c <= 57) or (c >= 65 and c <= 90) or (c >= 97 and c <= 122) or
    c == 33 or
    c == 35 or
    c == 36 or
    c == 37 or
    c == 38 or
    c == 39 or
    c == 42 or
    c == 43 or
    c == 45 or
    c == 46 or
    c == 94 or
    c == 95 or
    c == 96 or
    c == 124 or
    c == 126
end

fn line_end(bytes: List(Byte), start: Int, limit: Int) : Result(Int, HeaderProblem)
  var at = start
  while true
    c = try octet(bytes, at)
    if c == 13
      next = try octet(bytes, at + 1)
      return Error(Malformed) if next != 10
      return Ok(at)
    end
    return Error(Malformed) if c == 10 or c == 0 or c == 127 or (c < 32 and c != 9)
    return Error(TooLarge) if at - start >= limit
    at += 1
  end
  Error(Malformed)
end

fn ascii?(bytes: List(Byte), part: Span, expected: List(Int)) : Bool
  return false if part.size != expected.size
  for offset in 0..part.size
    c = case bytes.get(part.start + offset)
      Some(byte): byte.to_int
      None: return false
    end
    lower = if c >= 65 and c <= 90: c + 32 else: c
    return false if expected.get(offset) != Some(lower)
  end
  true
end

fn bearer_span(bytes: List(Byte), head: Head) : Option(Span)
  var found = None
  for field in head.fields
    if ascii?(bytes, field.name, [97, 117, 116, 104, 111, 114, 105, 122, 97, 116, 105, 111, 110])
      case found
        Some(_): return None
        None: found = Some(field.value)
      end
    end
  end
  value = case found
    Some(part): part
    None: return None
  end
  return None if value.size <= 6
  return None if not ascii?(bytes, Span(start: value.start, size: 6), [98, 101, 97, 114, 101, 114])
  var at = value.start + 6
  return None if octet(bytes, at) != Ok(32)
  finish = value.start + value.size
  while at < finish and octet(bytes, at) == Ok(32)
    at += 1
  end
  if at < finish: Some(Span(start: at, size: finish - at)) else: None
end

fn request_line(bytes: List(Byte), finish: Int) : Result((Span, Span, Span), HeaderProblem)
  var at = 0
  while at < finish
    c = try octet(bytes, at)
    break if c == 32
    return Error(Malformed) if not token?(c)
    at += 1
  end
  return Error(Malformed) if at == 0 or at >= finish
  method = Span(start: 0, size: at)
  at += 1
  target_start = at
  while at < finish
    c = try octet(bytes, at)
    break if c == 32
    return Error(Malformed) if c <= 32 or c >= 127
    at += 1
  end
  return Error(Malformed) if at == target_start or at >= finish
  target = Span(start: target_start, size: at - target_start)
  at += 1
  version_start = at
  while at < finish
    c = try octet(bytes, at)
    return Error(Malformed) if c <= 32 or c >= 127
    at += 1
  end
  return Error(Malformed) if at == version_start
  Ok((method, target, Span(start: version_start, size: at - version_start)))
end

fn field(bytes: List(Byte), start: Int, finish: Int) : Result(Field, HeaderProblem)
  var at = start
  while at < finish
    c = try octet(bytes, at)
    break if c == 58
    return Error(Malformed) if not token?(c)
    at += 1
  end
  return Error(Malformed) if at == start or at >= finish
  name = Span(start: start, size: at - start)
  at += 1
  while at < finish
    c = try octet(bytes, at)
    break if c != 32 and c != 9
    at += 1
  end
  var last = finish
  while last > at
    c = try octet(bytes, last - 1)
    break if c != 32 and c != 9
    last -= 1
  end
  Ok(Field(name: name, value: Span(start: at, size: last - at)))
end

fn parse_headers(bytes: List(Byte), limits: HeaderLimits) : Result(Head, HeaderProblem)
  return Error(TooLarge) if bytes.size > limits.bytes
  first = try line_end(bytes, 0, limits.line)
  (method, target, version) = try request_line(bytes, first)
  var fields = []
  var at = first + 2
  while true
    finish = try line_end(bytes, at, limits.line)
    if finish == at
      return Ok(
        Head(method: method, target: target, version: version, fields: fields, after: finish + 2)
      )
    end
    return Error(TooLarge) if fields.size >= limits.fields
    parsed = try field(bytes, at, finish)
    fields = fields.append(parsed)
    at = finish + 2
  end
  Error(Malformed)
end

fn skip_space(bytes: List(Byte), start: Int) : Int
  var at = start
  while octet(bytes, at) == Ok(32) or octet(bytes, at) == Ok(9)
    at += 1
  end
  at
end

fn chunk_size(bytes: List(Byte), maximum: Int, line_limit: Int) : Result(Int, HeaderProblem)
  return Error(TooLarge) if bytes.size > line_limit + 2
  finish = try line_end(bytes, 0, line_limit)
  return Error(Malformed) if finish + 2 != bytes.size
  var at = 0
  var size = 0
  while at < finish
    c = try octet(bytes, at)
    digit = if c >= 48 and c <= 57
      c - 48
    else if c >= 65 and c <= 70
      c - 55
    else if c >= 97 and c <= 102
      c - 87
    else
      -1
    end
    break if digit < 0
    return Error(TooLarge) if size > maximum / 16 or (size == maximum / 16 and digit > maximum % 16)
    size = size * 16 + digit
    at += 1
  end
  return Error(Malformed) if at == 0
  while at < finish
    at = skip_space(bytes, at)
    return Error(Malformed) if at >= finish or octet(bytes, at) != Ok(59)
    at = skip_space(bytes, at + 1)
    name_start = at
    while at < finish
      c = try octet(bytes, at)
      break if not token?(c)
      at += 1
    end
    return Error(Malformed) if at == name_start
    equal = skip_space(bytes, at)
    if equal < finish and octet(bytes, equal) == Ok(61)
      at = skip_space(bytes, equal + 1)
      return Error(Malformed) if at >= finish
      if octet(bytes, at) == Ok(34)
        at += 1
        while at < finish and octet(bytes, at) != Ok(34)
          if octet(bytes, at) == Ok(92)
            at += 1
            return Error(Malformed) if at >= finish
          end
          at += 1
        end
        return Error(Malformed) if at >= finish
        at += 1
      else
        value_start = at
        while at < finish
          c = try octet(bytes, at)
          break if not token?(c)
          at += 1
        end
        return Error(Malformed) if at == value_start
      end
    end
  end
  Ok(size)
end

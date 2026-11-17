## The same fetch as ../proposed/fetch.mo, using only the Bytes operations Mo has today: size,
## get and decode_utf8. Every byte of the response becomes a list cell, and the body is decoded
## from UTF-8 in Mo. The decoder does not validate: it is here for its cost.
##
##   fetch PORT PATH
module Fetch
use Json

fn main(args: Args, console: Console, files: Files) : Int
  case args.list
    [port, path]: report(console, files, port, path)
    _:
      console.error("usage: fetch PORT PATH")
      2
  end
end

fn report(console: Console, files: Files, port: Text, path: Text) : Int
  case fetch(files, "tcp:#{port}", path)
    Error(reason):
      console.error("fetch: #{reason}")
      1
    Ok(body):
      case Json.parse(body)
        Error(problem):
          console.error("json: line #{problem.line}, column #{problem.column}")
          1
        Ok(Json.Array(items)):
          console.print("#{items.size} items, #{body.byte_count} bytes")
          0
        Ok(_):
          console.error("json: not an array")
          1
      end
  end
end

fn fetch(files: Files, target: Text, path: Text) : Result(Text, Text)
  request = "GET #{path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"
  try files.write_text(target, request).map_error(fn(e) "#{e}" end)
  var response = []
  while true
    part = try files.read_bytes(target).map_error(fn(e) "#{e}" end)
    break if part.size == 0
    for i in 0..part.size
      response = response.append(byte_at(part, i))
    end
  end
  head_end = try header_end(response).or_fail("no end of headers")
  head = try text_of(response, 0, head_end).or_fail("headers are not text")
  lines = head.split("\r\n")
  status = (lines.first or "").split(" ").get(1) or ""
  return Error("status #{status}") if status != "200"
  headers = lines.drop(1).filter_map(fn(line) field(line) end).to_map
  if (headers.get("transfer-encoding") or "") == "chunked"
    payload = try dechunk(response, head_end + 4)
    text_of(payload, 0, payload.size).or_fail("body is not text")
  else
    length = try (headers.get("content-length") or "").to_int.or_fail("no content length")
    text_of(response, head_end + 4, head_end + 4 + length).or_fail("body is not text")
  end
end

fn field(line: Text) : Option((Text, Text))
  parts = line.split(":")
  name = try parts.first
  Some((name.trim.ascii_lowercase, parts.drop(1).join(":").trim))
end

fn header_end(bytes: List(Int)) : Option(Int)
  var at = 0
  while at + 3 < bytes.size
    if at_or(bytes, at) == 13 and
      at_or(bytes, at + 1) == 10 and
      at_or(bytes, at + 2) == 13 and
      at_or(bytes, at + 3) == 10
      return Some(at)
    end
    at += 1
  end
  None
end

fn byte_at(bytes: Bytes, at: Int) : Int
  case bytes.get(at)
    Some(b): b.to_int
    None: -1
  end
end

fn at_or(bytes: List(Int), at: Int) : Int
  bytes.get(at) or -1
end

fn dechunk(bytes: List(Int), start: Int) : Result(List(Int), Text)
  var at = start
  var payload = []
  while true
    var size = 0
    while at_or(bytes, at) != 13
      size = size * 16 + (try hex(at_or(bytes, at)).or_fail("bad chunk size"))
      at += 1
    end
    at += 2
    break if size == 0
    for i in at..at + size
      payload = payload.append(at_or(bytes, i))
    end
    at += size + 2
  end
  Ok(payload)
end

fn hex(c: Int) : Option(Int)
  return Some(c - 48) if c >= 48 and c <= 57
  return Some(c - 87) if c >= 97 and c <= 102
  return Some(c - 55) if c >= 65 and c <= 70
  None
end

fn text_of(bytes: List(Int), start: Int, after: Int) : Option(Text)
  var chars = []
  var at = start
  while at < after
    lead = at_or(bytes, at)
    following = if lead < 128: 0 else: following_bytes(lead)
    var code = lead - lead_bits(following)
    for i in 1..following + 1
      code = code * 64 + at_or(bytes, at + i) - 128
    end
    chars = chars.append(try code.to_char)
    at += following + 1
  end
  Some(Text.from_chars(chars))
end

fn following_bytes(lead: Int) : Int
  return 3 if lead >= 240
  return 2 if lead >= 224
  1
end

fn lead_bits(following: Int) : Int
  case following
    0: 0
    1: 192
    2: 224
    _: 240
  end
end

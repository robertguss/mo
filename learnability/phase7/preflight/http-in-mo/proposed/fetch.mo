## Fetches one JSON document over a plain connection, with HTTP parsed in Mo, and prints how
## many items it holds. Uses the two proposed operations, Bytes.slice and Bytes.join.
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
  var parts = []
  while true
    part = try files.read_bytes(target).map_error(fn(e) "#{e}" end)
    break if part.size == 0
    parts = parts.append(part)
  end
  response = Bytes.join(parts)
  head_end = try header_end(response).or_fail("no end of headers")
  head = try response.slice(0, head_end).decode_utf8.or_fail("headers are not text")
  lines = head.split("\r\n")
  status = (lines.first or "").split(" ").get(1) or ""
  return Error("status #{status}") if status != "200"
  headers = lines.drop(1).filter_map(fn(line) field(line) end).to_map
  payload = if (headers.get("transfer-encoding") or "") == "chunked"
    try dechunk(response, head_end + 4)
  else
    length = try (headers.get("content-length") or "").to_int.or_fail("no content length")
    response.slice(head_end + 4, head_end + 4 + length)
  end
  payload.decode_utf8.or_fail("body is not text")
end

fn field(line: Text) : Option((Text, Text))
  parts = line.split(":")
  name = try parts.first
  Some((name.trim.ascii_lowercase, parts.drop(1).join(":").trim))
end

fn header_end(bytes: Bytes) : Option(Int)
  var at = 0
  while at + 3 < bytes.size
    if byte_at(bytes, at) == 13 and
      byte_at(bytes, at + 1) == 10 and
      byte_at(bytes, at + 2) == 13 and
      byte_at(bytes, at + 3) == 10
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

fn dechunk(bytes: Bytes, start: Int) : Result(Bytes, Text)
  var at = start
  var parts = []
  while true
    var size = 0
    while byte_at(bytes, at) != 13
      size = size * 16 + (try hex(byte_at(bytes, at)).or_fail("bad chunk size"))
      at += 1
    end
    at += 2
    break if size == 0
    parts = parts.append(bytes.slice(at, at + size))
    at += size + 2
  end
  Ok(Bytes.join(parts))
end

fn hex(c: Int) : Option(Int)
  return Some(c - 48) if c >= 48 and c <= 57
  return Some(c - 87) if c >= 97 and c <= 102
  return Some(c - 55) if c >= 65 and c <= 70
  None
end

module HttpTransportCheck
use Http
use HttpAuthority
use HttpPolicy
use HttpBody

fn octets(text: Text) : List(Byte)
  text.chars.map(fn(c) c.code.to_byte or stop "non-ASCII fixture" end)
end

fn require(ok: Bool)
  if not ok
    stop "HTTP transport contract"
  end
end

fn head(bytes: List(Byte)) : Http.Head
  case Http.parse_headers(bytes, Http.HeaderLimits(bytes: 65536, line: 8192, fields: 64))
    Ok(value): value
    Error(_): stop "header fixture"
  end
end

fn authorities()
  for entry in [
    "localhost",
    "LOCALHOST:",
    "localhost:00080",
    "example.test.",
    "%65xample.test",
    "[::]",
    "[::1]",
    "[0:0:0:0:0:0:0:1]:80",
    "[2001:db8::1]:1234",
    "[::ffff:192.0.2.1]",
    "[1:2:3:4:5:6:192.0.2.1]",
    "host:999999999999999999999999"
  ]
    require(HttpAuthority.check([entry]))
  end
  for entry in [
    "",
    ":80",
    "user@localhost",
    "localhost:abc",
    "localhost:-1",
    "localhost:1:2",
    "localhost/x",
    "localhost?x",
    "localhost#x",
    "local host",
    "é",
    "%",
    "%0z",
    "[localhost]",
    "::1",
    "[::1",
    "[::1]extra",
    "[1:2:3:4:5:6:7]",
    "[1:2:3:4:5:6:7:8:9]",
    "[1:2:3:4:5:6:7:8::]",
    "[1::2::3]",
    "[:::1]",
    "[:1::]",
    "[::1:]",
    "[::ffff:999.1.1.1]",
    "[::ffff:01.1.1.1]",
    "[fe80::1%25eth0]"
  ]
    if HttpAuthority.check([entry])
      stop "accepted invalid authority"
    end
  end
  for (left, right, expected) in [
    ("LOCALHOST", "localhost:00080", true),
    ("localhost:", "localhost", true),
    ("[ABCD::1]", "[abcd::1]:80", true),
    ("host:00000", "HOST:0", true),
    ("host", "host:81", false),
    ("host", "host.", false),
    ("%68ost", "host", false),
    ("[::1]", "[0:0:0:0:0:0:0:1]", false),
    ("127.0.0.1", "127.1", false)
  ]
    a = HttpAuthority.parse(left) or stop "left authority"
    b = HttpAuthority.parse(right) or stop "right authority"
    require(HttpAuthority.equal?(a, b) == expected)
  end
  require(HttpAuthority.check([]))
  require(not HttpAuthority.check(["localhost", "bad host"]))
end

fn inspection()
  for (line, fields, expected) in [
    ("GET /bad HTTP/1.0", "", 505),
    ("POST /mcp http/1.1", "Host: localhost\r\n", 505),
    ("GET /bad HTTP/1.1", "Origin: x\r\n", 400),
    ("POST /mcp HTTP/1.1", "Host: localhost\r\nHost: localhost\r\n", 400),
    ("POST /mcp HTTP/1.1", "Host: user@localhost\r\n", 400),
    ("GET /bad HTTP/1.1", "Host: other\r\nOrigin: x\r\n", 421),
    ("GET /bad HTTP/1.1", "Host: localhost\r\nOrigin:\r\n", 403),
    ("GET /mcp?x HTTP/1.1", "Host: localhost\r\n", 404),
    ("GET /mcp HTTP/1.1", "Host: localhost\r\n", 405),
    ("post /mcp HTTP/1.1", "Host: localhost\r\n", 405),
    ("POST http://other/mcp HTTP/1.1", "Host: localhost\r\n", 421),
    ("POST http://localhost/mcp HTTP/1.1", "Host: user@localhost\r\n", 400),
    ("POST http://user@localhost/mcp HTTP/1.1", "Host: localhost\r\n", 400),
    ("POST http://localhost/mcp?x HTTP/1.1", "Host: localhost\r\n", 404),
    ("POST /mcp HTTP/1.1", "Host: localhost.\r\n", 421),
    ("POST /mcp HTTP/1.1", "Host: %6cocalhost\r\n", 421)
  ]
    bytes = octets("#{line}\r\n#{fields}\r\n")
    if HttpPolicy.inspect(bytes, head(bytes), ["localhost"]) != Error(expected)
      stop "inspection order"
    end
  end
  for (target, host) in [
    ("/mcp", "LOCALHOST:080"),
    ("http://localhost/mcp", "other"),
    ("HTTP://LOCALHOST:/mcp", "other:9000")
  ]
    bytes = octets("POST #{target} HTTP/1.1\r\nHost: #{host}\r\nContent-Length: -1\r\n\r\n")
    require(HttpPolicy.inspect(bytes, head(bytes), ["localhost"]) == Ok(None))
  end
  for auth in ["Bearer canary", "bEaReR  canary"]
    bytes = octets("POST /mcp HTTP/1.1\r\nHost: localhost\r\nAuthorization: #{auth}\r\n\r\n")
    parsed = head(bytes)
    require(HttpPolicy.inspect(bytes, parsed, ["localhost"]) == Ok(Http.bearer_span(bytes, parsed)))
  end
end

fn plan(fields: Text, limit: Int) : HttpPolicy.BodyPlan
  bytes = octets("POST /mcp HTTP/1.1\r\nHost: localhost\r\n#{fields}\r\n")
  HttpPolicy.body_plan(bytes, head(bytes), limit)
end

fn planning()
  json = "Content-Type: application/json\r\n"
  for (fields, status) in [
    ("Expect: something\r\n", 417),
    ("Expect: 100-continue;bad=yes\r\n", 417),
    ("Expect: 100-continue, other\r\n", 417),
    ("Content-Length: -1\r\n", 415),
    ("Content-Type: text/plain\r\nContent-Length: -1\r\n", 415),
    ("#{json}Content-Type: application/json\r\n", 415),
    ("#{json}Accept: text/event-stream\r\nContent-Encoding: gzip\r\n", 406),
    ("#{json}Content-Encoding: gzip\r\nContent-Length: -1\r\n", 415),
    ("#{json}Content-Length: 1\r\nTransfer-Encoding: chunked\r\n", 400),
    ("#{json}Content-Length: 1\r\nContent-Length: 1\r\n", 400),
    ("#{json}Content-Length: 1, 1\r\n", 400),
    ("#{json}Content-Length: -1\r\n", 400),
    ("#{json}Content-Length: +1\r\n", 400),
    ("#{json}Content-Length:\r\n", 400),
    ("#{json}Content-Length: 1x\r\n", 400),
    ("#{json}Content-Length: 9223372036854775808\r\n", 400),
    ("#{json}Content-Length: 9223372036854775807\r\n", 413),
    ("#{json}Content-Length: 3\r\nExpect: 100-continue\r\n", 413),
    ("#{json}Transfer-Encoding: gzip\r\n", 400),
    ("#{json}Transfer-Encoding: chunked, gzip\r\n", 400),
    ("#{json}Transfer-Encoding: gzip, chunked\r\n", 501),
    ("#{json}Transfer-Encoding: gzip;level=1, chunked\r\n", 501),
    ("#{json}Transfer-Encoding: chunked, chunked\r\n", 400),
    ("#{json}Transfer-Encoding: chunked;x=1\r\n", 400),
    ("#{json}Transfer-Encoding: chunked;\r\n", 400),
    ("#{json}Transfer-Encoding: chunked,\r\n", 400)
  ]
    if plan(fields, 2) != HttpPolicy.Reject(status)
      stop "body plan rejection"
    end
  end
  for accept in [
    "application/json",
    "application/json, text/event-stream",
    "text/event-stream, APPLICATION/JSON",
    "*/*",
    "application/*;q=0.1",
    "application/json;q=0.001, */*;q=0",
    "application/*;q=0, application/json;q=1",
    "*/*;q=1, application/json;other=x;q=0",
    "application/json;q=1;extension=yes"
  ]
    if plan("#{json}Accept: #{accept}\r\n", 0) !=
      HttpPolicy.Fixed(length: 0, send_continue: false, version: None)
      stop "JSON acceptance"
    end
  end
  for accept in [
    "",
    "text/event-stream",
    "application/json;q=0, */*;q=1",
    "application/*;q=0, */*;q=1",
    "application/json;q=1.001",
    "application/json;q=-1",
    "application/json;q=.5",
    "application/json;q=0.0001",
    "application/json;q=\"1\"",
    "application/json;q=1;q=0",
    "application/json;other=x"
  ]
    require(plan("#{json}Accept: #{accept}\r\n", 0) == HttpPolicy.Reject(406))
  end
  for content_type in [
    "application/json",
    "Application/JSON; charset=utf-8",
    "application/json; charset=\"utf-8\"",
    "application/json; profile=\"x,y;z\\\"q\""
  ]
    require(
      plan("Content-Type: #{content_type}\r\nContent-Length: 0002\r\nExpect: 100-continue\r\n", 2) ==
        HttpPolicy.Fixed(length: 2, send_continue: true, version: None)
    )
  end
  require(
    plan("#{json}Transfer-Encoding: CHUNKED\r\nExpect: 100-continue\r\n", 2) ==
      HttpPolicy.Chunked(send_continue: true, version: None)
  )
  for (version_fields, version) in [
    ("", None),
    ("MCP-Protocol-Version: 2025-11-25\r\n", Some("2025-11-25")),
    ("MCP-Protocol-Version: wrong\r\n", Some("")),
    ("MCP-Protocol-Version:\r\n", Some("")),
    ("MCP-Protocol-Version: 2025-11-25\r\nMCP-Protocol-Version: 2025-11-25\r\n", Some(""))
  ]
    require(
      plan("#{json}#{version_fields}", 0) ==
        HttpPolicy.Fixed(length: 0, send_continue: false, version: version)
    )
  end
end

fn framing(initial: HttpBody.State, fragments: List(List(Byte)), bounds: HttpBody.Bounds) : Result(
  (HttpBody.State, List(Byte), Int),
  Int
)
  var state = initial
  var collected = []
  var consumed = 0
  for fragment in fragments
    step = try HttpBody.feed(state, fragment, bounds)
    for span in step.payload
      require(span.start >= 0 and span.size > 0 and span.start + span.size <= step.consumed)
      collected = collected.concat(fragment.drop(span.start).take(span.size))
    end
    consumed += step.consumed
    state = step.state
  end
  Ok((state, collected, consumed))
end

fn bodies()
  bounds = HttpBody.Bounds(
    body: 256,
    ingress: 512,
    chunks: 16,
    line: 128,
    trailer_bytes: 128,
    trailer_fields: 4
  )
  wire = octets(
    "2;name=\"v\\\"x\"\r\nab\r\n3\r\ncde\r\n0\r\nX: y\r\nAuthorization: ignored\r\n\r\n"
  )
  input = wire.concat(octets("POST /mcp HTTP/1.1\r\n"))
  for split in 0..input.size + 1
    result = framing(HttpBody.chunked(), [input.take(split), input.drop(split)], bounds)
    case result
      Ok((state, payload, consumed)):
        require(state.phase == HttpBody.Complete)
        require(state.encoded == wire.size and state.decoded == 5 and state.chunks == 2)
        require(state.trailer_bytes == 32 and state.trailer_fields == 2)
        require(payload == octets("abcde"))
        require(consumed == wire.size)
      Error(_): stop "chunked fragment boundary"
    end
  end
  case framing(HttpBody.chunked(), input.map(fn(b) [b] end), bounds)
    Ok((state, payload, consumed)):
      require(state.phase == HttpBody.Complete and state.encoded == wire.size)
      require(payload == octets("abcde") and consumed == wire.size)
    Error(_): stop "single-byte chunked fragments"
  end
  for split in 0..8
    fixed = octets("abcdeXX")
    case framing(HttpBody.fixed(5), [fixed.take(split), fixed.drop(split)], bounds)
      Ok((state, payload, consumed)):
        require(state.phase == HttpBody.Complete and state.encoded == 5 and state.decoded == 5)
        require(payload == octets("abcde") and consumed == 5)
      Error(_): stop "fixed fragment boundary"
    end
  end
  binary = octets("3\r\n").append(0.to_byte or stop "NUL fixture").append(
    255.to_byte or stop "byte fixture"
  ).append(13.to_byte or stop "CR fixture").concat(octets("\r\n0\r\n\r\n"))
  case framing(HttpBody.chunked(), binary.map(fn(b) [b] end), bounds)
    Ok((state, payload, consumed)):
      require(state.phase == HttpBody.Complete and consumed == binary.size)
      require(payload == binary.drop(3).take(3))
    Error(_): stop "binary payload"
  end
  for malformed in [
    "\r\n",
    "-1\r\n",
    "0x1\r\n",
    "1;x=\r\n",
    "1\n",
    "1\rX",
    "1\r\naX",
    "1\r\na\n",
    "1\r\na\rX",
    "0\r\n folded: value\r\n\r\n",
    "0\r\n\tcontinued\r\n\r\n",
    "0\r\nName : x\r\n\r\n",
    "0\r\nNoColon\r\n\r\n",
    "0\r\nX: y\n\r\n",
    "0\r\nX: y\rX",
    "0\r\n: x\r\n\r\n"
  ]
    bytes = octets(malformed)
    for split in 0..bytes.size + 1
      require(
        framing(HttpBody.chunked(), [bytes.take(split), bytes.drop(split)], bounds) == Error(400)
      )
    end
    require(framing(HttpBody.chunked(), bytes.map(fn(b) [b] end), bounds) == Error(400))
  end
  for code in [0, 1, 31, 127]
    bad = octets("0\r\nX: ").append(code.to_byte or stop "control fixture").concat(
      octets("\r\n\r\n")
    )
    require(framing(HttpBody.chunked(), bad.map(fn(b) [b] end), bounds) == Error(400))
  end
  for partial in ["", "1", "1\r", "1\r\na", "1\r\na\r", "0\r\n", "0\r\nX: y\r\n"]
    case HttpBody.feed(HttpBody.chunked(), octets(partial), bounds)
      Ok(step): require(step.state.phase != HttpBody.Complete)
      Error(_): stop "incomplete chunked input"
    end
  end
  case HttpBody.feed(HttpBody.fixed(0), octets("NEXT"), bounds)
    Ok(step): require(
      step.consumed == 0 and step.payload.empty? and step.state.phase == HttpBody.Complete
    )
    Error(_): stop "empty body"
  end
end

fn body_limits()
  wire = octets("2;x=y\r\nab\r\n1\r\nc\r\n0\r\nX:y\r\n\r\n")
  exact = HttpBody.Bounds(
    body: 3,
    ingress: wire.size,
    chunks: 2,
    line: 5,
    trailer_bytes: 7,
    trailer_fields: 1
  )
  case HttpBody.feed(HttpBody.chunked(), wire.concat(octets("NEXT")), exact)
    Ok(step): require(step.state.phase == HttpBody.Complete and step.consumed == wire.size)
    Error(_): stop "body exact bounds"
  end
  for bounds in [
    HttpBody.Bounds(
      body: 2,
      ingress: wire.size,
      chunks: 2,
      line: 5,
      trailer_bytes: 7,
      trailer_fields: 1
    ),
    HttpBody.Bounds(
      body: 3,
      ingress: wire.size - 1,
      chunks: 2,
      line: 5,
      trailer_bytes: 7,
      trailer_fields: 1
    ),
    HttpBody.Bounds(
      body: 3,
      ingress: wire.size,
      chunks: 1,
      line: 5,
      trailer_bytes: 7,
      trailer_fields: 1
    ),
    HttpBody.Bounds(
      body: 3,
      ingress: wire.size,
      chunks: 2,
      line: 4,
      trailer_bytes: 7,
      trailer_fields: 1
    ),
    HttpBody.Bounds(
      body: 3,
      ingress: wire.size,
      chunks: 2,
      line: 5,
      trailer_bytes: 6,
      trailer_fields: 1
    ),
    HttpBody.Bounds(
      body: 3,
      ingress: wire.size,
      chunks: 2,
      line: 5,
      trailer_bytes: 7,
      trailer_fields: 0
    )
  ]
    require(HttpBody.feed(HttpBody.chunked(), wire, bounds) == Error(413))
    require(framing(HttpBody.chunked(), wire.map(fn(b) [b] end), bounds) == Error(413))
  end
  fixed_bounds = HttpBody.Bounds(
    body: 3,
    ingress: 3,
    chunks: 0,
    line: 0,
    trailer_bytes: 0,
    trailer_fields: 0
  )
  case HttpBody.feed(HttpBody.fixed(3), octets("abcNEXT"), fixed_bounds)
    Ok(step): require(step.consumed == 3 and step.state.phase == HttpBody.Complete)
    Error(_): stop "fixed ingress excludes suffix"
  end
  require(HttpBody.feed(HttpBody.fixed(4), octets("abcd"), fixed_bounds) == Error(413))
  ingress_bounds = HttpBody.Bounds(
    body: 4,
    ingress: 3,
    chunks: 0,
    line: 0,
    trailer_bytes: 0,
    trailer_fields: 0
  )
  require(HttpBody.feed(HttpBody.fixed(4), octets("abcd"), ingress_bounds) == Error(413))
  zero_bounds = HttpBody.Bounds(
    body: 0,
    ingress: 5,
    chunks: 0,
    line: 1,
    trailer_bytes: 2,
    trailer_fields: 0
  )
  case HttpBody.feed(HttpBody.chunked(), octets("0\r\n\r\nNEXT"), zero_bounds)
    Ok(step): require(
      step.consumed == 5 and step.state.chunks == 0 and step.state.phase == HttpBody.Complete
    )
    Error(_): stop "terminal chunk is not a data chunk"
  end
end

fn transport_limits()
  bounds = HttpBody.Bounds(
    body: 262144,
    ingress: 524288,
    chunks: 1024,
    line: 8192,
    trailer_bytes: 16384,
    trailer_fields: 64
  )
  body = octets("x".repeat(262144))
  case HttpBody.feed(HttpBody.fixed(262144), body.concat(octets("NEXT")), bounds)
    Ok(step):
      require(step.payload == [Http.Span(start: 0, size: 262144)])
      require(step.consumed == 262144 and step.state.phase == HttpBody.Complete)
    Error(_): stop "256 KiB body boundary"
  end
  require(HttpBody.feed(HttpBody.fixed(262145), body, bounds) == Error(413))
  for (count, expected) in [(1024, 0), (1025, 413)]
    bytes = octets("#{"1\r\nx\r\n".repeat(count)}0\r\n\r\n")
    case HttpBody.feed(HttpBody.chunked(), bytes, bounds)
      Ok(step): require(
        expected == 0 and step.state.phase == HttpBody.Complete and step.state.chunks == count
      )
      Error(code): require(code == expected)
    end
  end
  for (size, expected) in [(8192, 0), (8193, 413)]
    bytes = octets("0;x=#{"a".repeat(size - 4)}\r\n\r\n")
    case HttpBody.feed(HttpBody.chunked(), bytes, bounds)
      Ok(step): require(expected == 0 and step.state.phase == HttpBody.Complete)
      Error(code): require(code == expected)
    end
  end
  for (count, expected) in [(64, 0), (65, 413)]
    bytes = octets("0\r\n#{"X:y\r\n".repeat(count)}\r\n")
    case HttpBody.feed(HttpBody.chunked(), bytes, bounds)
      Ok(step): require(
        expected == 0 and
          step.state.phase == HttpBody.Complete and
          step.state.trailer_fields == count
      )
      Error(code): require(code == expected)
    end
  end
  for (size, expected) in [(8187, 0), (8188, 413)]
    bytes = octets("0\r\nX:#{"a".repeat(8187)}\r\nY:#{"b".repeat(size)}\r\n\r\n")
    case HttpBody.feed(HttpBody.chunked(), bytes, bounds)
      Ok(step): require(
        expected == 0 and
          step.state.phase == HttpBody.Complete and
          step.state.trailer_bytes == 16384
      )
      Error(code): require(code == expected)
    end
  end
  padded = "1;x=#{"a".repeat(503)}\r\nx\r\n"
  for (size, expected) in [(498, 0), (499, 413)]
    bytes = octets("#{padded.repeat(1023)}1;x=#{"a".repeat(size)}\r\nx\r\n0\r\n\r\n")
    require(bytes.size == 524288 + size - 498)
    case HttpBody.feed(HttpBody.chunked(), bytes, bounds)
      Ok(step):
        require(expected == 0 and step.state.phase == HttpBody.Complete)
        require(step.state.encoded == 524288 and step.state.decoded == 1024)
      Error(code): require(code == expected)
    end
  end
end

fn main() : Int
  authorities()
  inspection()
  planning()
  bodies()
  body_limits()
  transport_limits()
  37
end

module HttpCheck
use Http

fn octets(text: Text) : List(Byte)
  text.chars.map(fn(c) c.code.to_byte or stop "non-ASCII fixture" end)
end

fn require(ok: Bool)
  if not ok
    stop "HTTP parser contract"
  end
end

fn chunk_sizes()
  for (line, size) in [
    ("0\r\n", 0),
    ("000\r\n", 0),
    ("1aF\r\n", 431),
    ("0011;flag;name=value\r\n", 17),
    ("1 \t; \tname \t= \t\"a\\\"b\\\\c\";empty=\"\"\r\n", 1),
    ("2;flag\r\n", 2)
  ]
    input = octets(line)
    require(Http.chunk_size(input, size, input.size - 2) == Ok(size))
    require(Http.chunk_size(input, size, input.size - 3) == Error(Http.TooLarge))
    if size > 0
      require(Http.chunk_size(input, size - 1, 128) == Error(Http.TooLarge))
    end
  end
  for bad in [
    "\r\n",
    "-1\r\n",
    "+1\r\n",
    " 1\r\n",
    "0x1\r\n",
    "1 \r\n",
    "1;\r\n",
    "1;;x\r\n",
    "1;=x\r\n",
    "1;x=\r\n",
    "1;x= \r\n",
    "1;x=\"\r\n",
    "1;x=\"x\\\r\n",
    "1;x=\"x\"garbage\r\n",
    "1;x=y \r\n",
    "1;x=y=z\r\n",
    "1\n",
    "1\rX",
    "1\r\nX"
  ]
    require(Http.chunk_size(octets(bad), 1024, 128) == Error(Http.Malformed))
  end
  for partial in ["", "1", "1\r", "1;name=\"text\""]
    require(Http.chunk_size(octets(partial), 1024, 128) == Error(Http.Incomplete))
  end
  require(
    Http.chunk_size(octets("100000000000000000000000000000000\r\n"), 262144, 128) ==
      Error(Http.TooLarge)
  )
  for byte in [0, 1, 31, 127]
    value = byte.to_byte or stop "byte fixture"
    input = octets("1;x=\"").append(value).concat(octets("\"\r\n"))
    require(Http.chunk_size(input, 1, 128) == Error(Http.Malformed))
  end
  for byte in [9, 128, 255]
    value = byte.to_byte or stop "byte fixture"
    input = octets("1;x=\"\\").append(value).concat(octets("\"\r\n"))
    require(Http.chunk_size(input, 1, 128) == Ok(1))
  end
end

fn main() : Int
  chunk_sizes()
  text = "POST /mcp HTTP/1.1\r\nHost: localhost\r\nAuthorization:\tBearer canary-123 \t\r\n\r\n"
  input = octets(text)
  limits = Http.HeaderLimits(bytes: input.size, line: 128, fields: 2)
  head = case Http.parse_headers(input, limits)
    Ok(value): value
    Error(_): stop "valid header"
  end
  require(head.method == Http.Span(start: 0, size: 4))
  require(head.target == Http.Span(start: 5, size: 4))
  require(head.version == Http.Span(start: 10, size: 8))
  require(head.after == input.size)
  require(
    head.fields == [
      Http.Field(name: Http.Span(start: 20, size: 4), value: Http.Span(start: 26, size: 9)),
      Http.Field(name: Http.Span(start: 37, size: 13), value: Http.Span(start: 52, size: 17))
    ]
  )
  initial = Http.HeaderScan(matched: 0, size: 0, complete: false)
  complete = Http.HeaderScan(matched: 4, size: input.size, complete: true)
  for split in 0..input.size + 1
    partial = case Http.scan_headers(input.take(split), initial, input.size)
      Ok(value): value
      Error(_): stop "header fragment"
    end
    require(
      Http.scan_headers(input.drop(split).concat(octets("body")), partial, input.size) ==
        Ok(complete)
    )
  end
  require(Http.scan_headers(input, initial, input.size - 1) == Error(Http.TooLarge))
  require(
    Http.parse_headers(input, Http.HeaderLimits(bytes: input.size - 1, line: 128, fields: 2)) ==
      Error(Http.TooLarge)
  )
  require(
    Http.parse_headers(input, Http.HeaderLimits(bytes: input.size, line: 128, fields: 1)) ==
      Error(Http.TooLarge)
  )
  require(
    Http.parse_headers(input, Http.HeaderLimits(bytes: input.size, line: 17, fields: 2)) ==
      Error(Http.TooLarge)
  )
  bare = octets("POST /mcp HTTP/1.1\r\n\r\n")
  require(
    Http.parse_headers(bare, Http.HeaderLimits(bytes: 22, line: 18, fields: 1)) ==
      Ok(
        Http.Head(
          method: Http.Span(start: 0, size: 4),
          target: Http.Span(start: 5, size: 4),
          version: Http.Span(start: 10, size: 8),
          fields: [],
          after: 22
        )
      )
  )
  require(
    Http.parse_headers(bare, Http.HeaderLimits(bytes: 22, line: 17, fields: 1)) ==
      Error(Http.TooLarge)
  )
  require(Http.parse_headers(input.take(input.size - 1), limits) == Error(Http.Incomplete))
  for bad in [
    "POST /mcp HTTP/1.1\nHost: localhost\r\n\r\n",
    "POST /mcp HTTP/1.1\rXHost: localhost\r\n\r\n",
    "POST /mcp HTTP/1.1\r\nHost : localhost\r\n\r\n",
    "POST /mcp HTTP/1.1\r\n Host: localhost\r\n\r\n",
    "POST /mcp HTTP/1.1\r\nHost: localhost\r\n\tcontinued\r\n\r\n",
    "POST  /mcp HTTP/1.1\r\n\r\n",
    "POST /mcp\tHTTP/1.1\r\n\r\n"
  ]
    require(
      Http.parse_headers(octets(bad), Http.HeaderLimits(bytes: 256, line: 128, fields: 4)) ==
        Error(Http.Malformed)
    )
  end
  short = octets("POST /mcp HTTP/1.1\r\nX:\t \r\n\r\n")
  case Http.parse_headers(short, limits)
    Ok(value): require(
      value.fields == [
        Http.Field(name: Http.Span(start: 20, size: 1), value: Http.Span(start: 24, size: 0))
      ]
    )
    Error(_): stop "empty field"
  end
  require(Http.bearer_span(input, head) == Some(Http.Span(start: 59, size: 10)))
  for (value, expected) in [
    ("Bearer x", Some(Http.Span(start: 42, size: 1))),
    ("bEaReR   x", Some(Http.Span(start: 44, size: 1))),
    ("Bearer\tx", None),
    ("Bearer", None),
    ("Bearer ", None),
    ("Basic x", None),
    ("Bearerx", None)
  ]
    bytes = octets("POST /mcp HTTP/1.1\r\naUtHoRiZaTiOn: #{value}\r\n\r\n")
    parsed = case Http.parse_headers(bytes, Http.HeaderLimits(bytes: 256, line: 128, fields: 4))
      Ok(parsed_head): parsed_head
      Error(_): stop "authorization header"
    end
    require(Http.bearer_span(bytes, parsed) == expected)
  end
  for fields in [
    "",
    "Authorization: Bearer x\r\nAUTHORIZATION: Bearer x\r\n",
    "Authorization: Basic x\r\nAuthorization: Bearer x\r\n"
  ]
    bytes = octets("POST /mcp HTTP/1.1\r\n#{fields}\r\n")
    parsed = case Http.parse_headers(bytes, Http.HeaderLimits(bytes: 256, line: 128, fields: 4))
      Ok(value): value
      Error(_): stop "authorization cardinality"
    end
    require(Http.bearer_span(bytes, parsed) == None)
  end
  37
end

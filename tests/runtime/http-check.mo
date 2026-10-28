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

fn main() : Int
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
  37
end

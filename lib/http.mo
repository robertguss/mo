## An HTTP/1.1 client: a request built a step at a time and sent over a `Network`, the response
## that comes back, and a fake network's replies for a test.
module Http
expose Request(..),
Response(..),
HttpError(..),
get,
post,
request,
header,
headers_named,
query,
form,
json,
text,
within,
max_body,
send,
ok?,
reply,
replies
use Json

struct Request
  method: Text
  url: Text
  headers: List((Text, Text))
  body: Bytes
  timeout: Duration
  body_limit: Int
end

struct Response
  status: Int
  headers: List((Text, Text))
  body: Bytes
end

enum HttpError
  BadUrl(url: Text)
  BadHeader(name: Text)
  Unreachable(NetworkError)
  BadResponse(reason: Text)
  NotJson(Json.JsonError)
  TooLarge(limit: Int)
end

## Where a URL leads, and the path and query its request line asks for.
struct Target
  secure: Bool
  authority: Text
  host: Text
  port: Int
  path: Text
end

## A request as the bytes a connection sends, and where to send them.
struct Wire
  target: Target
  bytes: Bytes
end

## The most the client reads of a response's headers and of its body, in bytes.
struct Limits
  headers: Int
  body: Int
end

## How far a chunked body has been read: where the next chunk starts in what follows the
## headers, the data of the chunks before it, and how many bytes that data is.
struct Walk
  at: Int
  parts: List(Bytes)
  total: Int
end

## Why reading stopped short of a whole response: more bytes must come, and the walk says where
## a chunked body is read on from, or it can never be one.
enum Halt
  More(Walk)
  Bad(HttpError)
end

## A GET request for a URL, with no headers, no body, 30 seconds to finish in and a 16 MiB body
## limit.
fn get(url: Text) : Request
  examples
    get("http://example.org/a") ==
      Request(
        method: "GET",
        url: "http://example.org/a",
        headers: [],
        body: "".bytes,
        timeout: 30.seconds,
        body_limit: 16777216
      )
  end

  request("GET", url)
end

## A POST request for a URL.
fn post(url: Text) : Request
  examples
    post("http://example.org/a") == request("POST", "http://example.org/a")
  end

  request("POST", url)
end

## A request with any method, written as the server expects it, such as `DELETE`.
fn request(method: Text, url: Text) : Request
  examples
    request("DELETE", "http://example.org/a").method == "DELETE"
    request("DELETE", "http://example.org/a").url == "http://example.org/a"
    request("DELETE", "http://example.org/a").timeout == 30.seconds
    request("DELETE", "http://example.org/a").body_limit == 16777216
  end

  Request(
    method: method,
    url: url,
    headers: [],
    body: "".bytes,
    timeout: 30.seconds,
    body_limit: 16777216
  )
end

## The request with one more header. Headers are sent sorted by name, so the order of calls
## with different names changes nothing.
fn header(request: Request, name: Text, value: Text) : Request
  examples
    get("http://example.org/").header("Accept", "text/plain").header("Accept", "text/html").headers == [
      ("Accept", "text/plain"),
      ("Accept", "text/html")
    ]
  end

  var changed = request
  changed.headers = request.headers.append((name, value))
  changed
end

## The request with one more query parameter on its URL, percent-encoded.
fn query(request: Request, name: Text, value: Text) : Request
  examples
    get("http://example.org/a").query("state", "open").url == "http://example.org/a?state=open"
    get("http://example.org/a?b=1").query("q", "a b&c").url ==
      "http://example.org/a?b=1&q=a%20b%26c"
    get("http://example.org").query("né", "~é_-.").url == "http://example.org?n%C3%A9=~%C3%A9_-."
  end

  mark = if request.url.contains?("?"): "&" else: "?"
  var changed = request
  changed.url = "#{request.url}#{mark}#{encoded(name)}=#{encoded(value)}"
  changed
end

## The text with every byte but an ASCII letter, a digit, `-`, `.`, `_` and `~` written as `%`
## and two hexadecimal digits.
fn encoded(text: Text) : Text
  text.chars.map(fn(c) escaped(c) end).join("")
end

fn escaped(c: Char) : Text
  return c.to_text if c.ascii_letter? or c.ascii_digit? or "-._~".contains?(c.to_text)
  bytes = c.to_text.bytes
  (0..bytes.size).to_list.map(fn(i) percent(bytes, i) end).join("")
end

fn percent(bytes: Bytes, at: Int) : Text
  byte = case bytes.get(at)
    Some(b): b.to_int
    None: stop "the position is inside the bytes"
  end
  digits = "0123456789ABCDEF".chars
  high = digits.get(byte / 16) or '0'
  low = digits.get(byte % 16) or '0'
  "%#{high}#{low}"
end

## The request with `pairs` percent-encoded as its `application/x-www-form-urlencoded` body.
fn form(request: Request, pairs: List((Text, Text))) : Request
  examples
    post("http://example.org/token").form([
      ("grant_type", "client_credentials"),
      ("scope", "read write&admin")
    ]).body ==
      "grant_type=client_credentials&scope=read%20write%26admin".bytes
    post("http://example.org/token").json(Json.Null).form([("a", "1")]).headers == [
      ("Content-Type", "application/x-www-form-urlencoded")
    ]
    post("http://example.org/token").form([]).body == "".bytes
  end

  encoded_body = pairs.map(fn(p) "#{encoded(p.0)}=#{encoded(p.1)}" end).join("&")
  with_body(request, "application/x-www-form-urlencoded", encoded_body.bytes)
end

## The request with `value` as its body, written as JSON, and the content type that says so.
fn json(request: Request, value: Json) : Request
  examples
    post("http://example.org/").json(Json.Object({"n": Json.int(1)})).body == "{\"n\":1}".bytes
    post("http://example.org/").json(Json.Null).headers == [("Content-Type", "application/json")]
  end

  with_body(request, "application/json", value.to_text.bytes)
end

## The request with `text` as its body, and the content type that says it is UTF-8 text.
fn text(request: Request, text: Text) : Request
  examples
    post("http://example.org/").text("héllo").body == "héllo".bytes
    post("http://example.org/").json(Json.Null).header("A", "b").text("c").headers == [
      ("A", "b"),
      ("Content-Type", "text/plain; charset=utf-8")
    ]
  end

  with_body(request, "text/plain; charset=utf-8", text.bytes)
end

fn with_body(request: Request, kind: Text, body: Bytes) : Request
  kept = request.headers.filter(fn(h) h.0.ascii_lowercase != "content-type" end)
  var changed = request
  changed.headers = kept.append(("Content-Type", kind))
  changed.body = body
  changed
end

## The request with `timeout` for its whole exchange, in place of the 30 seconds it starts with.
fn within(request: Request, timeout: Duration) : Request
  examples
    get("http://example.org/").within(2.seconds).timeout == 2.seconds
  end

  var changed = request
  changed.timeout = timeout
  changed
end

## The request with `bytes` as the largest response body `send` accepts, in place of 16 MiB.
fn max_body(request: Request, bytes: Int) : Request
  examples
    get("http://example.org/").max_body(67108864).body_limit == 67108864
  end

  var changed = request
  changed.body_limit = bytes
  changed
end

## Makes the request over a new connection and gives the response, whatever its status. The
## request's timeout covers the whole exchange.
fn send(request: Request, network: Network) : Result(Response, HttpError)
  examples
    get("http://example.org/a").send(
      Network.fake(replies([(get("http://example.org/a"), reply(404, "gone"))]))
    ) ==
      Ok(reply(404, "gone"))
    get("https://example.org/a").send(
      Network.fake(replies([(get("https://example.org/a"), reply(200, "secure"))]))
    ) ==
      Ok(reply(200, "secure"))
    get("http://example.org/a").send(Network.fake({})) ==
      Error(Unreachable(Network.UnknownHost(host: "example.org")))
    get("example.org/a").send(Network.fake({})) == Error(BadUrl(url: "example.org/a"))
  end

  written = try wire(request)
  connection = if written.target.secure
    try network.connect_secure(written.target.host, written.target.port, request.timeout)
  else
    try network.connect(written.target.host, written.target.port, request.timeout)
  end
  try connection.send(written.bytes)
  bound = Limits(headers: 65536, body: request.body_limit)
  var head_bytes = "".bytes
  var head_from = 0
  var cut_found = None
  while cut_found is None
    part = try connection.receive(1048576)
    head_bytes = Bytes.join([head_bytes, part])
    case mark_at(
      head_bytes,
      "\r\n\r\n".bytes,
      head_from,
      bound.headers - head_from,
      "the headers are"
    )
      Ok(at): cut_found = Some(at)
      Error(Bad(_)): return Error(
        BadResponse(reason: "the headers are longer than #{bound.headers} bytes")
      )
      Error(More(_)):
        if part.size == 0
          return Error(BadResponse(reason: "the connection closed before the response was whole"))
        end
        head_from = (head_bytes.size - 3).max(0)
    end
  end
  cut = cut_found or 0
  head = try head_bytes.slice(0, cut).decode_utf8.or_fail(
    BadResponse(reason: "the headers are not text")
  )
  lines = head.split("\r\n")
  if not lines.all?(fn(line) plain?(line.replace("\t", " "), 32) end)
    return Error(BadResponse(reason: "a line holds a control character"))
  end
  status = try code(lines.first or "").or_fail(BadResponse(reason: "the status line is malformed"))
  headers = lines.drop(1).filter_map(fn(line) field(line) end)
  return Error(BadResponse(reason: "a header line is malformed")) if headers.size != lines.size - 1
  if request.method == "HEAD" or status == 204 or status == 304
    return Ok(Response(status: status, headers: headers, body: "".bytes))
  end
  first_rest = head_bytes.slice(cut + 4, head_bytes.size)
  codings = headers.filter(fn(h) h.0.ascii_lowercase == "transfer-encoding" end)
  if not codings.empty?
    coding = codings.map(fn(h) h.1 end).join(", ")
    if coding.ascii_lowercase != "chunked"
      return Error(BadResponse(reason: "the transfer encoding #{coding} is not supported"))
    end
    var unread = first_rest
    var walk = start()
    var closed = false
    while true
      case chunks(unread, bound, walk)
        Ok(body): return Ok(Response(status: status, headers: headers, body: body))
        Error(Bad(refusal)): return Error(refusal)
        Error(More(next)):
          if closed
            return Error(BadResponse(reason: "the connection closed before the response was whole"))
          end
          unread = unread.slice(next.at, unread.size)
          walk = Walk(at: 0, parts: next.parts, total: next.total)
          part = try connection.receive(1048576)
          if part.size == 0
            closed = true
          else
            unread = if unread.size == 0: part else: Bytes.join([unread, part])
          end
      end
    end
  end
  if named(headers, "content-length") is Some(written_length)
    length = try number(written_length).or_fail(
      BadResponse(reason: "the Content-Length is not a number")
    )
    return Error(TooLarge(limit: bound.body)) if length > bound.body
    var parts = if first_rest.size == 0: [] else: [first_rest]
    var received_size = first_rest.size
    while received_size < length
      part = try connection.receive(1048576)
      if part.size == 0
        return Error(BadResponse(reason: "the connection closed before the response was whole"))
      end
      parts = parts.append(part)
      received_size += part.size
    end
    body = Bytes.join(parts)
    return Ok(Response(status: status, headers: headers, body: body.slice(0, length)))
  end
  return Error(TooLarge(limit: bound.body)) if first_rest.size > bound.body
  var parts = if first_rest.size == 0: [] else: [first_rest]
  var received_size = first_rest.size
  while true
    part = try connection.receive(1048576)
    if part.size == 0
      return Ok(Response(status: status, headers: headers, body: Bytes.join(parts)))
    end
    received_size += part.size
    return Error(TooLarge(limit: bound.body)) if received_size > bound.body
    parts = parts.append(part)
  end
  Error(BadResponse(reason: "the response was never whole"))
end

## The limits `send` reads within: 64 KiB of headers and 16 MiB of body.
fn limits() : Limits
  Limits(headers: 65536, body: 16777216)
end

## Whether the status says the request succeeded: 200 to 299.
fn ok?(response: Response) : Bool
  examples
    reply(200, "").ok?
    reply(299, "").ok?
    not reply(199, "").ok?
    not reply(300, "").ok?
    not reply(404, "").ok?
  end

  response.status >= 200 and response.status < 300
end

## The value of the first header with this name, matched without regard to case.
fn header(response: Response, name: Text) : Option(Text)
  examples
    reply(200, "four").header("content-LENGTH") == Some("4")
    reply(200, "four").header("Content-Type") == None
  end

  named(response.headers, name.ascii_lowercase)
end

## Every header value with this name in response order, matched without regard to case.
fn headers_named(response: Response, name: Text) : List(Text)
  examples
    Response(
      status: 200,
      headers: [
        ("Link", "<https://api.example.org?page=2>; rel=\"next\""),
        ("Content-Type", "application/json"),
        ("link", "<https://api.example.org?page=5>; rel=\"last\"")
      ],
      body: "".bytes
    ).headers_named("LINK") == [
      "<https://api.example.org?page=2>; rel=\"next\"",
      "<https://api.example.org?page=5>; rel=\"last\""
    ]
    reply(200, "four").headers_named("Link") == []
  end

  lowered = name.ascii_lowercase
  response.headers.filter(fn(h) h.0.ascii_lowercase == lowered end).map(fn(h) h.1 end)
end

fn named(headers: List((Text, Text)), name: Text) : Option(Text)
  headers.find(fn(h) h.0.ascii_lowercase == name end).map(fn(h) h.1 end)
end

## The body parsed as JSON.
fn json(response: Response) : Result(Json, HttpError)
  examples
    reply(200, "[1]").json == Ok(Json.Array([Json.int(1)]))
    reply(200, "[1").json ==
      Error(NotJson(Json.JsonError(line: 1, column: 3, problem: Json.UnexpectedEnd)))
    Response(status: 200, headers: [], body: "é".bytes.slice(0, 1)).json ==
      Error(BadResponse(reason: "the body is not UTF-8 text"))
  end

  decoded = try response.body.decode_utf8.or_fail(BadResponse(reason: "the body is not UTF-8 text"))
  Ok(try Json.parse(decoded))
end

## The body as text, or `None` when it isn't UTF-8.
fn text(response: Response) : Option(Text)
  examples
    reply(200, "héllo").text == Some("héllo")
    Response(status: 200, headers: [], body: "é".bytes.slice(0, 1)).text == None
  end

  response.body.decode_utf8
end

## What the bytes received so far hold: a whole response, or `More` while more must come, with
## the walk to pass as `from` next time. `finished` says the other side has sent all it will,
## so a response still short is an error.
fn read(method: Text, received: Bytes, finished: Bool, limits: Limits, from: Walk) : Result(
  Response,
  Halt
)
  examples
    read(
      "GET",
      "HTTP/1.1 200 OK\r\nA: b\r\nContent-Length: 2\r\n\r\nhi!".bytes,
      false,
      limits(),
      start()
    ) ==
      Ok(Response(status: 200, headers: [("A", "b"), ("Content-Length", "2")], body: "hi".bytes))
    read("GET", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nh".bytes, false, limits(), start()) ==
      Error(More(start()))
    read("GET", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nh".bytes, true, limits(), start()) ==
      Error(bad("the connection closed before the response was whole"))
    read("GET", "HTTP/1.1 200 OK\r\nContent-Len".bytes, false, limits(), start()) ==
      Error(More(start()))
    read("GET", "HTTP/2 200\r\n\r\n".bytes, false, limits(), start()) ==
      Error(bad("the status line is malformed"))
    read("GET", "\r\n\r\n".bytes, false, limits(), start()) ==
      Error(bad("the status line is malformed"))
    read("GET", "HTTP/1.1 200 OK\r\nno colon\r\n\r\n".bytes, false, limits(), start()) ==
      Error(bad("a header line is malformed"))
    read("GET", "HTTP/1.1 200 OK\r\nA: é".bytes.slice(0, 21), true, limits(), start()) ==
      Error(bad("the connection closed before the response was whole"))
    read(
      "GET",
      Bytes.join(["HTTP/1.1 200 OK\r\nA: ".bytes, "é".bytes.slice(0, 1), "\r\n\r\n".bytes]),
      true,
      limits(),
      start()
    ) ==
      Error(bad("the headers are not text"))
    read("HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n".bytes, false, limits(), start()) ==
      Ok(Response(status: 200, headers: [("Content-Length", "5")], body: "".bytes))
    read("GET", "HTTP/1.1 204 No Content\r\n\r\n".bytes, false, limits(), start()) ==
      Ok(Response(status: 204, headers: [], body: "".bytes))
    read("GET", "HTTP/1.1 304 Not Modified\r\n\r\n".bytes, false, limits(), start()) ==
      Ok(Response(status: 304, headers: [], body: "".bytes))
    read("GET", "HTTP/1.1 200 OK\r\n\r\nhi".bytes, false, limits(), start()) == Error(More(start()))
    read("GET", "HTTP/1.1 200 OK\r\n\r\nhi".bytes, true, limits(), start()) ==
      Ok(Response(status: 200, headers: [], body: "hi".bytes))
    read("GET", "HTTP/1.1 200 OK\r\n\r\nhi".bytes, true, Limits(headers: 19, body: 2), start()) ==
      Ok(Response(status: 200, headers: [], body: "hi".bytes))
    read("GET", "HTTP/1.1 200 OK\r\n\r\nhi".bytes, true, Limits(headers: 18, body: 2), start()) ==
      Error(bad("the headers are longer than 18 bytes"))
    read("GET", "HTTP/1.1 200 OK\r\n\r\nhi!".bytes, false, Limits(headers: 19, body: 2), start()) ==
      Error(Bad(TooLarge(limit: 2)))
    read(
      "GET",
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi".bytes,
      false,
      Limits(headers: 64, body: 2),
      start()
    ) ==
      Ok(Response(status: 200, headers: [("Content-Length", "2")], body: "hi".bytes))
    read(
      "GET",
      "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n".bytes,
      false,
      Limits(headers: 64, body: 2),
      start()
    ) ==
      Error(Bad(TooLarge(limit: 2)))
    read("GET", "HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\n".bytes, false, limits(), start()) ==
      Error(bad("the Content-Length is not a number"))
    read(
      "GET",
      "HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\nhello".bytes,
      false,
      limits(),
      start()
    ) ==
      Error(bad("the Content-Length is not a number"))
    read(
      "GET",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\nContent-Length: 9\r\n\r\n2\r\nhi\r\n0\r\n\r\n"
        .bytes,
      false,
      limits(),
      start()
    ) ==
      Ok(
        Response(
          status: 200,
          headers: [("Transfer-Encoding", "Chunked"), ("Content-Length", "9")],
          body: "hi".bytes
        )
      )
    read(
      "GET",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n".bytes,
      false,
      limits(),
      start()
    ) ==
      Error(bad("the transfer encoding gzip is not supported"))
    read("GET", "HTTP/1.1 200 OK\nContent-Length: 2\n\nhi".bytes, true, limits(), start()) ==
      Error(bad("the connection closed before the response was whole"))
    read("GET", "HTTP/1.1 200 OK\nContent-Length: 2\r\n\r\nhi".bytes, true, limits(), start()) ==
      Error(bad("a line holds a control character"))
    read("GET", "HTTP/1.1 200 OK\r\nA: b\nC: d\r\n\r\n".bytes, true, limits(), start()) ==
      Error(bad("a line holds a control character"))
    read("GET", "HTTP/1.1 200 OK\r\nA: b\u{0}c\r\n\r\n".bytes, true, limits(), start()) ==
      Error(bad("a line holds a control character"))
    read("GET", "HTTP/1.1 200 OK\r\nA: b\u{7f}c\r\n\r\n".bytes, true, limits(), start()) ==
      Error(bad("a line holds a control character"))
    read("GET", "HTTP/1.1 200 OK\r\nA: b\tc\r\n\r\n".bytes, true, limits(), start()) ==
      Ok(Response(status: 200, headers: [("A", "b\tc")], body: "".bytes))
    read(
      "GET",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: gzip\r\n\r\n0\r\n\r\n"
        .bytes,
      false,
      limits(),
      start()
    ) ==
      Error(bad("the transfer encoding chunked, gzip is not supported"))
    read(
      "GET",
      "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n\r\n".bytes,
      true,
      limits(),
      start()
    ) ==
      Ok(Response(status: 100, headers: [], body: "HTTP/1.1 200 OK\r\n\r\n".bytes))
  end

  found = whole(method, received, finished, limits, from)
  if finished and found is Error(More(_))
    Error(bad("the connection closed before the response was whole"))
  else
    found
  end
end

## The walk of a chunked body none of which has been read.
fn start() : Walk
  Walk(at: 0, parts: [], total: 0)
end

fn bad(reason: Text) : Halt
  Bad(BadResponse(reason: reason))
end

fn whole(method: Text, received: Bytes, finished: Bool, limits: Limits, from: Walk) : Result(
  Response,
  Halt
)
  cut = try mark_at(received, "\r\n\r\n".bytes, 0, limits.headers, "the headers are")
  head = try received.slice(0, cut).decode_utf8.or_fail(bad("the headers are not text"))
  lines = head.split("\r\n")
  if not lines.all?(fn(line) plain?(line.replace("\t", " "), 32) end)
    return Error(bad("a line holds a control character"))
  end
  status = try code(lines.first or "").or_fail(bad("the status line is malformed"))
  headers = lines.drop(1).filter_map(fn(line) field(line) end)
  return Error(bad("a header line is malformed")) if headers.size != lines.size - 1
  body = if method == "HEAD" or status == 204 or status == 304
    "".bytes
  else
    try content(headers, received.slice(cut + 4, received.size), finished, limits, from)
  end
  Ok(Response(status: status, headers: headers, body: body))
end

## Where `mark` first starts at or after `from`, when it ends within `most` bytes of `from`.
## `what` starts the reason when it doesn't, as in "the headers are".
fn mark_at(received: Bytes, mark: Bytes, from: Int, most: Int, what: Text) : Result(Int, Halt)
  examples
    mark_at("ab\r\n".bytes, "\r\n".bytes, 0, 4, "a line is") == Ok(2)
    mark_at("ab\r\ncd\r\n".bytes, "\r\n".bytes, 3, 5, "a line is") == Ok(6)
    mark_at("ab\r\n".bytes, "\r\n".bytes, 0, 3, "a line is") ==
      Error(Bad(BadResponse(reason: "a line is longer than 3 bytes")))
    mark_at("abc".bytes, "\r\n".bytes, 0, 3, "a line is") ==
      Error(Bad(BadResponse(reason: "a line is longer than 3 bytes")))
    mark_at("ab".bytes, "\r\n".bytes, 0, 3, "a line is") == Error(More(start()))
    mark_at("ab\r".bytes, "\r\n".bytes, 1, 3, "a line is") == Error(More(start()))
  end

  last = received.size.min(from + most) - mark.size
  var at = from
  while at <= last
    return Ok(at) if received.slice(at, at + mark.size) == mark
    at += 1
  end
  return Error(bad("#{what} longer than #{most} bytes")) if received.size >= from + most
  Error(More(start()))
end

## The status in a status line such as `HTTP/1.1 200 OK`: three digits after the version.
fn code(line: Text) : Option(Int)
  examples
    code("HTTP/1.1 200 OK") == Some(200)
    code("HTTP/1.0 404") == Some(404)
    code("HTTP/1.1 2000 OK") == None
    code("HTTP/1.1 20 OK") == None
    code("HTTP/1.1 OK 200") == None
    code("HTTP/1.1") == None
    code("HTTP/2 200 OK") == None
    code("200 OK") == None
  end

  parts = line.split(" ")
  return None if not (parts.first or "").starts_with?("HTTP/1.")
  digits = try parts.get(1)
  return None if digits.byte_count != 3
  number(digits)
end

## A header line as its name and its value, or `None` for a line with no colon or with a name
## that is empty or holds white space.
fn field(line: Text) : Option((Text, Text))
  examples
    field("Date: 12:30") == Some(("Date", "12:30"))
    field("X:") == Some(("X", ""))
    field("no colon") == None
    field(": b") == None
    field("A b: c") == None
    field(" A: b") == None
  end

  parts = line.split(":")
  name = try parts.first
  return None if parts.size < 2 or name.words != [name]
  Some((name, parts.drop(1).join(":").trim))
end

## The body in what followed the headers, by the framing the headers name: chunks, a length,
## or everything until the other side has finished.
fn content(headers: List((Text, Text)), rest: Bytes, finished: Bool, limits: Limits, from: Walk) : Result(
  Bytes,
  Halt
)
  codings = headers.filter(fn(h) h.0.ascii_lowercase == "transfer-encoding" end)
  if not codings.empty?
    coding = codings.map(fn(h) h.1 end).join(", ")
    if coding.ascii_lowercase != "chunked"
      return Error(bad("the transfer encoding #{coding} is not supported"))
    end
    return chunks(rest, limits, from)
  end
  if named(headers, "content-length") is Some(written)
    length = try number(written).or_fail(bad("the Content-Length is not a number"))
    return Error(Bad(TooLarge(limit: limits.body))) if length > limits.body
    return Error(More(from)) if rest.size < length
    return Ok(rest.slice(0, length))
  end
  return Error(Bad(TooLarge(limit: limits.body))) if rest.size > limits.body
  return Error(More(from)) if not finished
  Ok(rest)
end

## The data of a chunked body, once its last chunk and the line that ends its trailers are in.
## It reads on from the walk an earlier call stopped with, so no chunk is read twice.
fn chunks(rest: Bytes, limits: Limits, from: Walk) : Result(Bytes, Halt)
  examples
    chunks("3\r\nabc\r\n0\r\n\r\n".bytes, Limits(headers: 8, body: 3), start()) == Ok("abc".bytes)
    chunks("1;x=y\r\na\r\n2\r\nbc\r\n0\r\nA: b\r\n\r\n".bytes, limits(), start()) == Ok("abc".bytes)
    chunks("0\r\n\r\n".bytes, limits(), start()) == Ok("".bytes)
    chunks("3\r\nabc\r\n0\r\n".bytes, limits(), start()) ==
      Error(More(Walk(at: 8, parts: ["abc".bytes], total: 3)))
    chunks("3\r\nabc\r\n".bytes, limits(), start()) ==
      Error(More(Walk(at: 8, parts: ["abc".bytes], total: 3)))
    chunks("3\r\nabc\r\n2\r\nd".bytes, limits(), start()) ==
      Error(More(Walk(at: 8, parts: ["abc".bytes], total: 3)))
    chunks("3\r\nabc\r".bytes, limits(), start()) == Error(More(start()))
    chunks(
      "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n".bytes,
      Limits(headers: 8, body: 5),
      Walk(at: 8, parts: ["abc".bytes], total: 3)
    ) ==
      Ok("abcde".bytes)
    chunks(
      "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n".bytes,
      Limits(headers: 8, body: 4),
      Walk(at: 8, parts: ["abc".bytes], total: 3)
    ) ==
      Error(Bad(TooLarge(limit: 4)))
    chunks("3\r\nabcde".bytes, limits(), start()) ==
      Error(Bad(BadResponse(reason: "a chunk's data doesn't end with a line break")))
    chunks("-3\r\nabc\r\n0\r\n\r\n".bytes, limits(), start()) ==
      Error(Bad(BadResponse(reason: "a chunk's size is malformed")))
    chunks("2\r\nab\r\n2\r\ncd\r\n0\r\n\r\n".bytes, Limits(headers: 8, body: 3), start()) ==
      Error(Bad(TooLarge(limit: 3)))
    chunks("2\r\nab\r\n2\r\nc".bytes, Limits(headers: 8, body: 3), start()) ==
      Error(Bad(TooLarge(limit: 3)))
    chunks("0\r\nA: b\r\n\r\n".bytes, Limits(headers: 8, body: 3), start()) ==
      Error(Bad(BadResponse(reason: "the trailers are longer than 8 bytes")))
    chunks("1;#{"x".repeat(252)}\r\na\r\n0\r\n\r\n".bytes, limits(), start()) == Ok("a".bytes)
    chunks("1;#{"x".repeat(253)}\r\na\r\n0\r\n\r\n".bytes, limits(), start()) ==
      Error(Bad(BadResponse(reason: "a chunk's size line is longer than 256 bytes")))
  end

  var at = from.at
  var parts = from.parts
  var total = from.total
  var last = false
  while not last
    line = mark_at(rest, "\r\n".bytes, at, 256, "a chunk's size line is")
    cut = try held(line, at, parts, total)
    size = try chunk_size(rest.slice(at, cut)).or_fail(bad("a chunk's size is malformed"))
    return Error(Bad(TooLarge(limit: limits.body))) if total + size > limits.body
    last = size == 0
    if not last
      data = cut + 2
      after = data + size
      return Error(More(Walk(at: at, parts: parts, total: total))) if rest.size < after + 2
      if rest.slice(after, after + 2) != "\r\n".bytes
        return Error(bad("a chunk's data doesn't end with a line break"))
      end
      parts = parts.append(rest.slice(data, after))
      total += size
      at = after + 2
    end
  end
  trailers = mark_at(rest, "\r\n\r\n".bytes, at, limits.headers, "the trailers are")
  _end = try held(trailers, at, parts, total)
  Ok(Bytes.join(parts))
end

## `found`, with the walk so far in place of an empty one when more bytes must come.
fn held(found: Result(Int, Halt), at: Int, parts: List(Bytes), total: Int) : Result(Int, Halt)
  examples
    held(Ok(4), 8, [], 3) == Ok(4)
    held(Error(More(start())), 8, ["abc".bytes], 3) ==
      Error(More(Walk(at: 8, parts: ["abc".bytes], total: 3)))
    held(Error(bad("no")), 8, [], 3) == Error(bad("no"))
  end

  if found is Error(More(_))
    Error(More(Walk(at: at, parts: parts, total: total)))
  else
    found
  end
end

## The size a chunk's first line gives, in hexadecimal, before any `;` and extension.
fn chunk_size(line: Bytes) : Option(Int)
  examples
    chunk_size("1a".bytes) == Some(26)
    chunk_size("0 ; name=value".bytes) == Some(0)
    chunk_size(" 1a".bytes) == None
    chunk_size("".bytes) == None
    chunk_size("é".bytes.slice(0, 1)) == None
  end

  written = try line.decode_utf8
  hex((written.split(";").first or "").trim_end)
end

## The number written in 1 to 8 hexadecimal digits, with no sign.
fn hex(text: Text) : Option(Int)
  examples
    hex("0") == Some(0)
    hex("1a") == Some(26)
    hex("FF") == Some(255)
    hex("0000000a") == Some(10)
    hex("00000000a") == None
    hex("") == None
    hex("1g") == None
    hex("-1") == None
  end

  return None if text.empty? or text.byte_count > 8
  var value = 0
  for c in text.chars
    value = value * 16 + (try hex_digit(c.ascii_lowercase))
  end
  Some(value)
end

fn hex_digit(c: Char) : Option(Int)
  return Some(c.code - 48) if c.ascii_digit?
  return Some(c.code - 87) if "abcdef".contains?(c.to_text)
  None
end

## A response for a test: a status, and a body with its length as the one header.
fn reply(status: Int, body: Text) : Response
  examples
    reply(404, "gone") ==
      Response(status: 404, headers: [("Content-Length", "4")], body: "gone".bytes)
    reply(200, "é").headers == [("Content-Length", "2")]
  end

  Response(status: status, headers: [("Content-Length", "#{body.byte_count}")], body: body.bytes)
end

## What `Network.fake` takes, for a test: each request as the client writes it, with the response
## it gets. A request the client would refuse to send is a bug.
fn replies(exchanges: List((Request, Response))) : Map(Text, Map(Bytes, Bytes))
  examples
    replies([]) == {}
    replies([(get("http://example.org/a"), reply(200, "hi"))]) == {
      "example.org:80": {
        "GET /a HTTP/1.1\r\nHost: example.org\r\nConnection: close\r\n\r\n".bytes: "HTTP/1.1 200 \r\nContent-Length: 2\r\n\r\nhi"
          .bytes
      }
    }
  end

  var peers = {}
  for (asked, answer) in exchanges
    case wire(asked)
      Ok(written):
        peer = "#{written.target.host}:#{written.target.port}"
        held = (peers.get(peer) or {}).set(written.bytes, answered(answer))
        peers = peers.set(peer, held)
      Error(_): stop "Http.replies was given a request the client refuses to send"
    end
  end
  peers
end

## A response as the bytes a server sends: its headers and body as they are.
fn answered(response: Response) : Bytes
  message("HTTP/1.1 #{response.status} ", response.headers, response.body)
end

fn message(first: Text, headers: List((Text, Text)), body: Bytes) : Bytes
  lines = headers.map(fn(h) "#{h.0}: #{h.1}\r\n" end)
  Bytes.join(["#{first}\r\n#{lines.join("")}\r\n".bytes, body])
end

## The request as bytes: the client's own headers first, then the program's sorted by name
## without regard to case. Nothing the program passes can add a line.
fn wire(request: Request) : Result(Wire, HttpError)
  examples
    wire(get("ftp://example.org/")) == Error(BadUrl(url: "ftp://example.org/"))
    wire(request("GE T", "http://example.org/")) == Error(BadHeader(name: "GE T"))
    wire(get("http://example.org/").header("A", "b").header("X\n", "y")) ==
      Error(BadHeader(name: "X\n"))
  end

  target = try parsed(request.url).or_fail(BadUrl(url: request.url))
  return Error(BadHeader(name: request.method)) if not word?(request.method)
  if request.headers.find(fn(h) not sendable?(h.0, h.1) end) is Some(refused)
    return Error(BadHeader(name: refused.0))
  end
  length = if sized?(request): [("Content-Length", "#{request.body.size}")] else: []
  sorted = request.headers.sort_by(fn(h) h.0.ascii_lowercase end)
  headers = [("Host", target.authority), ("Connection", "close")].concat(length).concat(sorted)
  first = "#{request.method} #{target.path} HTTP/1.1"
  Ok(Wire(target: target, bytes: message(first, headers, request.body)))
end

## Whether the request says how long its body is: always, but for a GET or a HEAD with none.
fn sized?(request: Request) : Bool
  request.body.size > 0 or not ["GET", "HEAD"].contains?(request.method)
end

## Whether the text is ASCII letters and nothing else, as a method is.
fn word?(text: Text) : Bool
  examples
    word?("PATCH")
    not word?("")
    not word?("GE T")
  end

  not text.empty? and text.chars.all?(fn(c) c.ascii_letter? end)
end

## Whether the client sends this header: a name with no space, colon or control character, a
## value with no control character, and not one of the four headers the client writes itself.
fn sendable?(name: Text, value: Text) : Bool
  examples
    sendable?("X-Token!", "Bearer t0ken")
    not sendable?("", "v")
    not sendable?("X Y", "v")
    not sendable?("X:Y", "v")
    not sendable?("X", "v\r\nHost: evil")
    not sendable?("X", "v\u{7f}")
    not sendable?("Content-Length", "0")
    not sendable?("Transfer-Encoding", "chunked")
    not sendable?("connection", "keep-alive")
    not sendable?("HOST", "evil")
  end

  plain?(name, 33) and
    not name.empty? and
    not name.contains?(":") and
    plain?(value, 32) and
    not ["host", "connection", "content-length", "transfer-encoding"].contains?(
      name.ascii_lowercase
    )
end

## Whether every character is at or above `lowest` and isn't the control character DEL.
fn plain?(text: Text, lowest: Int) : Bool
  text.chars.all?(fn(c) c.code >= lowest and c.code != 127 end)
end

## Where a URL leads: `http://` or `https://`, a host, then an optional port, path and query.
## `None` for anything else, a space or a control character anywhere, or a port outside 1 to
## 65,535.
fn parsed(url: Text) : Option(Target)
  examples
    parsed("http://example.org") ==
      Some(
        Target(secure: false, authority: "example.org", host: "example.org", port: 80, path: "/")
      )
    parsed("https://example.org/a!b?c=d") ==
      Some(
        Target(
          secure: true,
          authority: "example.org",
          host: "example.org",
          port: 443,
          path: "/a!b?c=d"
        )
      )
    parsed("http://example.org:8080?c=d/e") ==
      Some(
        Target(
          secure: false,
          authority: "example.org:8080",
          host: "example.org",
          port: 8080,
          path: "/?c=d/e"
        )
      )
    parsed("http://a:1/").map(fn(t) t.port end) == Some(1)
    parsed("http://a:65535/").map(fn(t) t.port end) == Some(65535)
    parsed("http://a:0/") == None
    parsed("http://a:65536/") == None
    parsed("http://a:http/") == None
    parsed("http://a:/") == None
    parsed("http://a:1:2/") == None
    parsed("http://:80/") == None
    parsed("http:///a") == None
    parsed("http://user@a/") == None
    parsed("ftp://a/") == None
    parsed("a/") == None
    parsed("http://a/b c") == None
    parsed("http://a b/") == None
    parsed("http://a/b\r\nX: y") == None
    parsed("http://a/b#c") == None
  end

  return None if not plain?(url, 33) or url.contains?("#")
  (secure, rest) = try address(url)
  authority = (rest.split("?").first or "").split("/").first or ""
  parts = authority.split(":")
  host = try parts.first
  return None if host.empty? or host.contains?("@") or parts.size > 2
  port = case parts.get(1)
    Some(written): try number(written)
    None: if secure: 443 else: 80
  end
  return None if port < 1 or port > 65535
  path = rest.drop_prefix(authority) or ""
  Some(
    Target(
      secure: secure,
      authority: authority,
      host: host,
      port: port,
      path: if path.starts_with?("/"): path else: "/#{path}"
    )
  )
end

## Whether the URL is `https`, and what follows its scheme. `None` for any other scheme.
fn address(url: Text) : Option((Bool, Text))
  case url.drop_prefix("http://")
    Some(rest): Some((false, rest))
    None: url.drop_prefix("https://").map(fn(rest) (true, rest) end)
  end
end

## The number written in ASCII digits alone. A sign is not a digit.
fn number(text: Text) : Option(Int)
  examples
    number("80") == Some(80)
    number("-80") == None
    number("+80") == None
    number("") == None
    number("99999999999999999999") == None
  end

  return None if not text.chars.all?(fn(c) c.ascii_digit? end)
  text.to_int
end

test "a request is written with the client's headers first and the program's sorted by name"
  asked = post("http://example.org:8080/items").query("state", "open").header("b-second", "2")
    .header("Authorization", "Bearer t0ken").header("B-Second", "1").json(
      Json.Object({"n": Json.int(1)})
    )
  assert wire(asked) is Ok(written)
  assert written.target.host == "example.org"
  assert written.target.port == 8080
  assert written.bytes.decode_utf8 ==
    Some(
      "POST /items?state=open HTTP/1.1\r\nHost: example.org:8080\r\nConnection: close\r\nContent-Length: 7\r\nAuthorization: Bearer t0ken\r\nb-second: 2\r\nB-Second: 1\r\nContent-Type: application/json\r\n\r\n{\"n\":1}"
    )
end

test "the order of header calls with different names never changes what is sent"
  one = get("http://example.org/").header("Accept", "a").header("X-Id", "7")
  other = get("http://example.org/").header("X-Id", "7").header("Accept", "a")
  assert one != other
  assert wire(one) is Ok(first)
  assert wire(other) is Ok(second)
  assert first.bytes == second.bytes
end

test "a body's length is written, and so is none for a method that carries one"
  assert wire(get("http://example.org/")) is Ok(bare)
  assert bare.bytes == "GET / HTTP/1.1\r\nHost: example.org\r\nConnection: close\r\n\r\n".bytes
  assert wire(request("HEAD", "http://example.org/")) is Ok(head)
  assert head.bytes == "HEAD / HTTP/1.1\r\nHost: example.org\r\nConnection: close\r\n\r\n".bytes
  assert wire(get("http://example.org/").text("x")) is Ok(odd)
  assert odd.bytes.decode_utf8 ==
    Some(
      "GET / HTTP/1.1\r\nHost: example.org\r\nConnection: close\r\nContent-Length: 1\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nx"
    )
  assert wire(post("http://example.org/")) is Ok(empty)
  assert empty.bytes.decode_utf8 ==
    Some("POST / HTTP/1.1\r\nHost: example.org\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")
end

test "nothing a program passes adds a line to the request"
  assert wire(get("http://example.org/a\r\nX: y")) ==
    Error(BadUrl(url: "http://example.org/a\r\nX: y"))
  assert wire(get("http://exam ple.org/")) == Error(BadUrl(url: "http://exam ple.org/"))
  assert wire(request("GET / HTTP/1.1\r\nX:", "http://example.org/")) ==
    Error(BadHeader(name: "GET / HTTP/1.1\r\nX:"))
  token = "t0ken\n"
  assert wire(get("http://example.org/").header("Authorization", token)) ==
    Error(BadHeader(name: "Authorization"))
  assert wire(get("http://example.org/").header("Host", "other.example")) ==
    Error(BadHeader(name: "Host"))
end

test "replies gives a fake one peer for each host and port, holding each request written"
  first = get("http://example.org/a")
  second = get("http://example.org/b").header("Accept", "text/plain")
  elsewhere = get("https://example.com/")
  peers = replies([
    (first, reply(200, "one")),
    (second, reply(404, "")),
    (elsewhere, reply(204, ""))
  ])
  assert peers.keys == ["example.org:80", "example.com:443"]
  assert (peers.get("example.org:80") or {}).size == 2
  network = Network.fake(peers)
  assert wire(second) is Ok(written)
  assert network.connect("example.org", 80, 1.second) is Ok(connection)
  assert connection.send(written.bytes) == Ok(Unit)
  assert connection.receive(1024) == Ok("HTTP/1.1 404 \r\nContent-Length: 0\r\n\r\n".bytes)
end

test "a function that calls an API is tested with no network"
  page = "[{\"number\": 7, \"title\": \"Crash on empty input\"}, {\"number\": 9, \"title\": \"Slow start\"}]"
  url = "http://api.example.com/repos/mo/mo/issues"
  asked = get(url).query("state", "open").header("Authorization", "Bearer t0ken")
  network = Network.fake(replies([(asked, reply(200, page))]))
  assert get(url).header("Authorization", "Bearer t0ken").query("state", "open").send(network) is
    Ok(answer)
  assert answer.ok?
  assert answer.json is Ok(issues)
  assert issues.text_at("1.title") == Some("Slow start")
  assert issues.int_at("0.number") == Some(7)
end

test "a request the fake holds no reply for is an error that quotes it, and so is https"
  held = get("http://example.org/a")
  network = Network.fake(replies([(held, reply(200, "one"))]))
  assert get("http://example.org/b").send(network) ==
    Error(Unreachable(Network.Broken(reason: "no reply for \"GET /b HTTP/1.1\"")))
  assert held.header("Accept", "a").send(network) ==
    Error(
      Unreachable(
        Network.Broken(
          reason: "no reply for \"GET /a HTTP/1.1\": sent \"Accept: a\\r\\n\", expected \"\\r\\n\""
        )
      )
    )
  assert held.send(network) == Ok(reply(200, "one"))
  secure = get("https://example.org/a")
  assert secure.send(Network.fake(replies([(secure, reply(200, "one"))]))) == Ok(reply(200, "one"))
  assert secure.send(Network.fake({})) ==
    Error(Unreachable(Network.UnknownHost(host: "example.org")))
end

test "a response is read whole however it says where it ends"
  asked = get("http://example.org/")
  long = "x".repeat(1100000)
  assert asked.send(Network.fake(replies([(asked, reply(200, long))]))) == Ok(reply(200, long))
  chunked = Response(
    status: 200,
    headers: [("Transfer-Encoding", "chunked")],
    body: "4;note=1\r\nWiki\r\n5\r\npedia\r\n0\r\nExpires: never\r\n\r\n".bytes
  )
  assert asked.send(Network.fake(replies([(asked, chunked)]))) is Ok(joined)
  assert joined.text == Some("Wikipedia")
  big_chunk = "y".repeat(65536)
  big_chunked = Response(
    status: 200,
    headers: [("Transfer-Encoding", "chunked")],
    body: "10000\r\n#{big_chunk}\r\n10000\r\n#{big_chunk}\r\n0\r\n\r\n".bytes
  )
  assert asked.send(Network.fake(replies([(asked, big_chunked)]))) is Ok(big_joined)
  assert big_joined.body.size == 131072
  closed = Response(status: 500, headers: [], body: "to the end".bytes)
  assert asked.send(Network.fake(replies([(asked, closed)]))) == Ok(closed)
  assert not closed.ok?
end

test "a chunked response read as each byte arrives is read on from where the walk stopped"
  encoded = "4;note=1\r\nWiki\r\n5\r\npedia\r\n0\r\nExpires: never\r\n\r\n".bytes
  all = answered(Response(status: 200, headers: [("Transfer-Encoding", "chunked")], body: encoded))
  var walk = start()
  for size in 1..all.size
    assert read("GET", all.slice(0, size), false, limits(), walk) is Error(More(next))
    walk = next
  end
  assert walk == Walk(at: 26, parts: ["Wiki".bytes, "pedia".bytes], total: 9)
  assert read("GET", all, false, limits(), walk) is Ok(response)
  assert response.body == "Wikipedia".bytes
end

test "a response that is cut off or past a limit is an error"
  asked = get("http://example.org/")
  short = Response(status: 200, headers: [("Content-Length", "5")], body: "hi".bytes)
  assert asked.send(Network.fake(replies([(asked, short)]))) ==
    Error(BadResponse(reason: "the connection closed before the response was whole"))
  huge = Response(status: 200, headers: [("Content-Length", "16777217")], body: "".bytes)
  assert asked.send(Network.fake(replies([(asked, huge)]))) == Error(TooLarge(limit: 16777216))
  full = Response(status: 200, headers: [("Content-Length", "16777216")], body: "".bytes)
  assert asked.send(Network.fake(replies([(asked, full)]))) ==
    Error(BadResponse(reason: "the connection closed before the response was whole"))
  padded = Response(status: 200, headers: [("X-Pad", "x".repeat(65511))], body: "".bytes)
  assert answered(padded).size == 65537
  assert asked.send(Network.fake(replies([(asked, padded)]))) ==
    Error(BadResponse(reason: "the headers are longer than 65536 bytes"))
  assert asked.max_body(2).send(Network.fake(replies([(asked, reply(200, "two"))]))) ==
    Error(TooLarge(limit: 2))
  assert asked.max_body(3).send(Network.fake(replies([(asked, reply(200, "two"))]))) ==
    Ok(reply(200, "two"))
  over_default = Response(status: 200, headers: [("Content-Length", "16777217")], body: "".bytes)
  assert asked.max_body(33554432).send(Network.fake(replies([(asked, over_default)]))) ==
    Error(BadResponse(reason: "the connection closed before the response was whole"))
end

test "form requests and repeated Link pagination headers round-trip over Network.fake"
  token_req = post("https://lms.example.edu/login/oauth2/token").form([
    ("grant_type", "refresh_token"),
    ("client_id", "keel-mo"),
    ("refresh_token", "r/1+2=3")
  ])
  page_req = get("https://lms.example.edu/api/v1/courses").header("Authorization", "Bearer tok")
  page_res = Response(
    status: 200,
    headers: [
      ("Link", "<https://lms.example.edu/api/v1/courses?page=2>; rel=\"next\""),
      ("link", "<https://lms.example.edu/api/v1/courses?page=9>; rel=\"last\""),
      ("Content-Length", "2")
    ],
    body: "[]".bytes
  )
  network = Network.fake(
    replies([(token_req, reply(200, "{\"access_token\":\"tok\"}")), (page_req, page_res)])
  )
  assert token_req.send(network) is Ok(tok_ans)
  assert tok_ans.json is Ok(tok_doc)
  assert tok_doc.text_at("access_token") == Some("tok")
  assert page_req.send(network) is Ok(courses_ans)
  assert courses_ans.header("Link") ==
    Some("<https://lms.example.edu/api/v1/courses?page=2>; rel=\"next\"")
  assert courses_ans.headers_named("link") == [
    "<https://lms.example.edu/api/v1/courses?page=2>; rel=\"next\"",
    "<https://lms.example.edu/api/v1/courses?page=9>; rel=\"last\""
  ]
end

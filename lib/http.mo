## An HTTP/1.1 client: a request built a step at a time, and a fake network's replies for a test.
module Http
expose Request(..),
Response(..),
HttpError(..),
get,
post,
request,
header,
query,
json,
text,
within,
reply,
replies
use Json

struct Request
  method: Text
  url: Text
  headers: List((Text, Text))
  body: Bytes
  timeout: Duration
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

## A GET request for a URL, with no headers, no body and 30 seconds to finish in.
fn get(url: Text) : Request
  examples
    get("http://example.org/a") ==
      Request(
        method: "GET",
        url: "http://example.org/a",
        headers: [],
        body: "".bytes,
        timeout: 30.seconds
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
  end

  Request(method: method, url: url, headers: [], body: "".bytes, timeout: 30.seconds)
end

## The request with one more header. Headers are sent sorted by name, so the order of these
## calls changes nothing.
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

## A response for a test: a status, and a body with its length as the one header.
fn reply(status: Int, body: Text) : Response
  examples
    reply(404, "gone") ==
      Response(status: 404, headers: [("Content-Length", "4")], body: "gone".bytes)
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

## Whether the URL is `https`, and what follows its scheme.
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

test "the order of header calls never changes what is sent"
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

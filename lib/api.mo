## Declarative HTTP/REST API request, response, routing and validation helpers.
module Api
expose Request(..),
Response(..),
request,
get,
post,
put,
patch,
delete,
with_header,
with_bearer,
with_query,
with_json,
with_text,
with_bytes,
header,
bearer_token,
query_param,
text,
ok?,
json,
response,
json_response,
text_response,
html_response,
no_content,
redirect,
sse_response,
percent_decode,
parse_query_string,
match_path,
parse_path_int,
parse_path_byte,
parse_path_bigint,
parse_path_decimal,
parse_path_bool,
parse_path_text,
require_query_int,
require_query_byte,
require_query_bigint,
require_query_decimal,
require_query_float,
require_query_bool,
require_query_text,
optional_query_int,
optional_query_byte,
optional_query_bigint,
optional_query_decimal,
optional_query_float,
optional_query_bool,
optional_query_text,
encoded_json_response,
body_decode_error,
domain_error_response,
not_found_response,
method_not_allowed_response,
sqlite_open_error_response,
cors_preflight,
apply_cors,
openapi_docs_html,
render_json
use Json

struct Request
  method: Text
  path: Text
  query: List((Text, Text))
  headers: List((Text, Text))
  body: Bytes
end

struct Response
  status: Int
  headers: List((Text, Text))
  body: Bytes
end

fn hex_nibble(b: Int) : Option(Int)
  return Some(b - 48) if b >= 48 and b <= 57
  return Some(b - 55) if b >= 65 and b <= 70
  return Some(b - 87) if b >= 97 and b <= 102
  None
end

## Decodes a percent-encoded UTF-8 string (`%XX`), optionally turning `+` into `' '`.
fn percent_decode(raw: Text, plus_space?: Bool) : Option(Text)
  examples
    percent_decode("hello%20world", false) == Some("hello world")
    percent_decode("a+b%26c", true) == Some("a b&c")
    percent_decode("n%C3%A9", false) == Some("né")
    percent_decode("%ZZ", false) == None
    percent_decode("%C3", false) == None
  end

  src = raw.bytes
  n = src.size
  var decoded = []
  var i = 0
  while i < n
    b = (try src.get(i)).to_int
    if b == 37
      return None if i + 2 >= n
      hi = try hex_nibble((try src.get(i + 1)).to_int)
      lo = try hex_nibble((try src.get(i + 2)).to_int)
      decoded = decoded.append(hi * 16 + lo)
      i += 3
    else if plus_space? and b == 43
      decoded = decoded.append(32)
      i += 1
    else
      decoded = decoded.append(b)
      i += 1
    end
  end
  m = decoded.size
  var chars = []
  var at = 0
  while at < m
    b0 = try decoded.get(at)
    if b0 < 128
      chars = chars.append(try b0.to_char)
      at += 1
    else if b0 >= 194 and b0 <= 223
      return None if at + 1 >= m
      b1 = try decoded.get(at + 1)
      return None if b1 < 128 or b1 > 191
      cp = (b0 - 192) * 64 + (b1 - 128)
      chars = chars.append(try cp.to_char)
      at += 2
    else if b0 >= 224 and b0 <= 239
      return None if at + 2 >= m
      b1 = try decoded.get(at + 1)
      b2 = try decoded.get(at + 2)
      return None if b1 < 128 or b1 > 191 or b2 < 128 or b2 > 191
      cp = (b0 - 224) * 4096 + (b1 - 128) * 64 + (b2 - 128)
      return None if cp < 2048
      chars = chars.append(try cp.to_char)
      at += 3
    else if b0 >= 240 and b0 <= 244
      return None if at + 3 >= m
      b1 = try decoded.get(at + 1)
      b2 = try decoded.get(at + 2)
      b3 = try decoded.get(at + 3)
      return None if b1 < 128 or b1 > 191 or b2 < 128 or b2 > 191 or b3 < 128 or b3 > 191
      cp = (b0 - 240) * 262144 + (b1 - 128) * 4096 + (b2 - 128) * 64 + (b3 - 128)
      return None if cp < 65536
      chars = chars.append(try cp.to_char)
      at += 4
    else
      return None
    end
  end
  Some(Text.from_chars(chars))
end

## Parses a raw query string (`a=1&b=two%20words`) into decoded `(key, value)` pairs.
fn parse_query_string(raw: Text) : List((Text, Text))
  examples
    parse_query_string("") == []
    parse_query_string("limit=10&q=a+b") == [("limit", "10"), ("q", "a b")]
  end

  return [] if raw.empty?
  var out = []
  for part in raw.split("&")
    continue if part.empty?
    pieces = part.split("=")
    k_raw = pieces.first or ""
    v_raw = if pieces.size > 1: pieces.drop(1).join("=") else: ""
    k = percent_decode(k_raw, true) or k_raw
    v = percent_decode(v_raw, true) or v_raw
    out = out.append((k, v))
  end
  out
end

fn split_target(target: Text) : (Text, List((Text, Text)))
  parts = target.split("?")
  raw_path = parts.first or "/"
  norm_path = if raw_path.starts_with?("/"): raw_path else: "/#{raw_path}"
  q = if parts.size > 1: parse_query_string(parts.drop(1).join("?")) else: []
  (norm_path, q)
end

## Builds an incoming API request with method and path (splitting any `?query` suffix).
fn request(method: Text, target: Text) : Request
  examples
    request("GET", "/users?limit=5").path == "/users"
    request("GET", "/users?limit=5").query == [("limit", "5")]
  end

  (p, q) = split_target(target)
  Request(method: method.ascii_uppercase, path: p, query: q, headers: [], body: "".bytes)
end

fn get(target: Text) : Request
  request("GET", target)
end

fn post(target: Text) : Request
  request("POST", target)
end

fn put(target: Text) : Request
  request("PUT", target)
end

fn patch(target: Text) : Request
  request("PATCH", target)
end

fn delete(target: Text) : Request
  request("DELETE", target)
end

fn replace_ci_header(headers: List((Text, Text)), name: Text, value: Text) : List((Text, Text))
  low = name.ascii_lowercase
  headers.filter(fn(h) h.0.ascii_lowercase != low end).append((name, value))
end

## Adds or replaces a header on a request.
fn with_header(req: Request, name: Text, value: Text) : Request
  var next = req
  next.headers = replace_ci_header(req.headers, name, value)
  next
end

## Sets `Authorization: Bearer <token>` on a request.
fn with_bearer(req: Request, token: Text) : Request
  with_header(req, "Authorization", "Bearer #{token}")
end

## Appends a query parameter to a request.
fn with_query(req: Request, name: Text, value: Text) : Request
  var next = req
  next.query = req.query.append((name, value))
  next
end

## Sets a JSON-encoded request body using the `Json` ability and `Content-Type: application/json`.
fn with_json(req: Request, value: T) : Request where T: Json
  with_bytes(req, Json.encode(value).bytes, "application/json")
end

## Sets a UTF-8 text request body and `Content-Type: text/plain; charset=utf-8`.
fn with_text(req: Request, body: Text) : Request
  with_bytes(req, body.bytes, "text/plain; charset=utf-8")
end

## Sets a raw `Bytes` request body and `Content-Type`.
fn with_bytes(req: Request, body: Bytes, content_type: Text) : Request
  var next = req
  next.headers = replace_ci_header(req.headers, "Content-Type", content_type)
  next.body = body
  next
end

## Reads a header from a request, ignoring ASCII case.
fn header(req: Request, name: Text) : Option(Text)
  low = name.ascii_lowercase
  req.headers.find(fn(h) h.0.ascii_lowercase == low end).map(fn(h) h.1 end)
end

## Extracts a bearer token from `Authorization: Bearer <token>`.
fn bearer_token(req: Request) : Option(Text)
  examples
    get("/me").with_bearer("s3cr3t").bearer_token == Some("s3cr3t")
    get("/me").with_header("authorization", "bearer abc").bearer_token == Some("abc")
    get("/me").bearer_token == None
  end

  raw = (try header(req, "Authorization")).trim
  return None if raw.byte_count < 8
  case raw.drop_prefix("Bearer ")
    Some(tok):
      trimmed = tok.trim
      if trimmed.empty?: None else: Some(trimmed)
    None:
      case raw.drop_prefix("bearer ")
        Some(tok):
          trimmed = tok.trim
          if trimmed.empty?: None else: Some(trimmed)
        None: None
      end
  end
end

## Reads a query parameter by exact name.
fn query_param(req: Request, name: Text) : Option(Text)
  req.query.find(fn(p) p.0 == name end).map(fn(p) p.1 end)
end

## Decodes the request body as UTF-8 text.
fn text(req: Request) : Option(Text)
  req.body.decode_utf8
end

## Builds a custom HTTP response.
fn response(status: Int, headers: List((Text, Text)), body: Bytes) : Response
  Response(status: status, headers: headers, body: body)
end

## Builds a JSON response from any value whose type has `Json`.
fn json_response(status: Int, value: T) : Response where T: Json
  encoded_json_response(status, value.to_json)
end

## Builds a plain-text UTF-8 response.
fn text_response(status: Int, body: Text) : Response
  Response(
    status: status,
    headers: [("Content-Type", "text/plain; charset=utf-8")],
    body: body.bytes
  )
end

## Builds an HTML UTF-8 response.
fn html_response(status: Int, html: Text) : Response
  Response(
    status: status,
    headers: [("Content-Type", "text/html; charset=utf-8")],
    body: html.bytes
  )
end

## Builds a `204 No Content` response.
fn no_content() : Response
  Response(status: 204, headers: [], body: "".bytes)
end

## Builds a `302 Found` redirect response.
fn redirect(location: Text) : Response
  Response(status: 302, headers: [("Location", location)], body: "".bytes)
end

fn format_sse_frame(event: (Option(Text), Text)) : Text
  head = case event.0
    Some(name): "event: #{name}\n"
    None: ""
  end
  data_lines = event.1.lines
  body = if data_lines.empty?
    "data: \n"
  else
    data_lines.map(fn(line) "data: #{line}\n" end).join("")
  end
  "#{head}#{body}\n"
end

## Builds a `200 OK` Server-Sent Events (`text/event-stream`) response.
fn sse_response(events: List((Option(Text), Text))) : Response
  payload = events.map(format_sse_frame).join("")
  Response(
    status: 200,
    headers: [("Content-Type", "text/event-stream"), ("Cache-Control", "no-cache")],
    body: payload.bytes
  )
end

## Adds or replaces a header on a response.
fn with_header(res: Response, name: Text, value: Text) : Response
  var next = res
  next.headers = replace_ci_header(res.headers, name, value)
  next
end

## Whether a response has a 2xx status code.
fn ok?(res: Response) : Bool
  200 <= res.status and res.status < 300
end

## Reads a header from a response, ignoring ASCII case.
fn header(res: Response, name: Text) : Option(Text)
  low = name.ascii_lowercase
  res.headers.find(fn(h) h.0.ascii_lowercase == low end).map(fn(h) h.1 end)
end

## Decodes a response's body as UTF-8 text (or empty string if non-UTF-8).
fn text(res: Response) : Text
  res.body.decode_utf8 or ""
end

## Parses a response's body as dynamic `Json`.
fn json(res: Response) : Result(Json, Json.JsonError)
  Json.parse(res.body.decode_utf8 or "")
end

fn path_segments(p: Text) : List(Text)
  trimmed = if p.byte_count > 1 and p.ends_with?("/")
    p.drop_suffix("/") or p
  else
    p
  end
  return [] if trimmed == "/" or trimmed.empty?
  rest = trimmed.drop_prefix("/") or trimmed
  rest.split("/")
end

## Matches a route template (`/users/:id`) against a request path, returning decoded `:param` values.
fn match_path(pattern: Text, actual_path: Text) : Option(List(Text))
  examples
    match_path("/health", "/health") == Some([])
    match_path("/users/:id", "/users/42") == Some(["42"])
    match_path("/users/:id", "/users/42/") == Some(["42"])
    match_path("/files/:name", "/files/hello%20world") == Some(["hello world"])
    match_path("/users/:id", "/posts/42") == None
    match_path("/users/:id", "/users/42/extra") == None
  end

  pat_segs = path_segments(pattern)
  act_segs = path_segments(actual_path)
  return None if pat_segs.size != act_segs.size
  var captured = []
  var i = 0
  while i < pat_segs.size
    pat = pat_segs.get(i) or ""
    act = act_segs.get(i) or ""
    if pat.starts_with?(":")
      return None if act.empty?
      val = try percent_decode(act, false)
      captured = captured.append(val)
    else
      dec = try percent_decode(act, false)
      return None if dec != pat
    end
    i += 1
  end
  Some(captured)
end

## Renders a `Json` value to compact UTF-8 text.
fn render_json(body_json: Json) : Text
  body_json.to_text
end

## Builds a JSON response from an already-constructed `Json` value.
fn encoded_json_response(status: Int, body_json: Json) : Response
  Response(
    status: status,
    headers: [("Content-Type", "application/json")],
    body: body_json.to_text.bytes
  )
end

fn validation_failure(pointer: Text, reason: Text) : Response
  encoded_json_response(
    422,
    Json.Object({
      "error": Json.String("validation_error"),
      "pointer": Json.String(pointer),
      "reason": Json.String(reason)
    })
  )
end

fn parse_big_scalar(raw: Text) : Option(BigInt)
  return None if raw.empty?
  ok_chars = raw.chars.all?(fn(c) c.ascii_digit? or c == '-' end)
  return None if not ok_chars
  case Json.decode[BigInt]("\"#{raw}\"")
    Ok(v): Some(v)
    Error(_): None
  end
end

fn parse_bool_scalar(raw: Text) : Option(Bool)
  if raw == "true"
    Some(true)
  else if raw == "false"
    Some(false)
  else
    None
  end
end

fn parse_byte_scalar(raw: Text) : Option(Byte)
  n = try raw.to_int
  return None if n < 0 or n > 255
  n.to_byte
end

fn parse_path_int(raw: Text, name: Text) : Result(Int, Response)
  case raw.to_int
    Some(v): Ok(v)
    None: Error(validation_failure("/path/#{name}", "expected integer"))
  end
end

fn parse_path_byte(raw: Text, name: Text) : Result(Byte, Response)
  case parse_byte_scalar(raw)
    Some(v): Ok(v)
    None: Error(validation_failure("/path/#{name}", "expected byte 0..255"))
  end
end

fn parse_path_bigint(raw: Text, name: Text) : Result(BigInt, Response)
  case parse_big_scalar(raw)
    Some(v): Ok(v)
    None: Error(validation_failure("/path/#{name}", "expected integer"))
  end
end

fn parse_path_decimal(raw: Text, name: Text) : Result(Decimal, Response)
  case raw.to_decimal
    Some(v): Ok(v)
    None: Error(validation_failure("/path/#{name}", "expected decimal"))
  end
end

fn parse_path_bool(raw: Text, name: Text) : Result(Bool, Response)
  case parse_bool_scalar(raw)
    Some(v): Ok(v)
    None: Error(validation_failure("/path/#{name}", "expected true or false"))
  end
end

fn parse_path_text(raw: Text, _name: Text) : Result(Text, Response)
  Ok(raw)
end

fn require_query_raw(req: Request, name: Text) : Result(Text, Response)
  case query_param(req, name)
    Some(v): Ok(v)
    None: Error(validation_failure("/query/#{name}", "missing required query parameter"))
  end
end

fn require_query_int(req: Request, name: Text) : Result(Int, Response)
  raw = try require_query_raw(req, name)
  case raw.to_int
    Some(v): Ok(v)
    None: Error(validation_failure("/query/#{name}", "expected integer"))
  end
end

fn require_query_byte(req: Request, name: Text) : Result(Byte, Response)
  raw = try require_query_raw(req, name)
  case parse_byte_scalar(raw)
    Some(v): Ok(v)
    None: Error(validation_failure("/query/#{name}", "expected byte 0..255"))
  end
end

fn require_query_bigint(req: Request, name: Text) : Result(BigInt, Response)
  raw = try require_query_raw(req, name)
  case parse_big_scalar(raw)
    Some(v): Ok(v)
    None: Error(validation_failure("/query/#{name}", "expected integer"))
  end
end

fn require_query_decimal(req: Request, name: Text) : Result(Decimal, Response)
  raw = try require_query_raw(req, name)
  case raw.to_decimal
    Some(v): Ok(v)
    None: Error(validation_failure("/query/#{name}", "expected decimal"))
  end
end

fn require_query_float(req: Request, name: Text) : Result(Float, Response)
  raw = try require_query_raw(req, name)
  case raw.to_float
    Some(v):
      if v.nan? or v.infinite?
        Error(validation_failure("/query/#{name}", "expected finite float"))
      else
        Ok(v)
      end
    None: Error(validation_failure("/query/#{name}", "expected float"))
  end
end

fn require_query_bool(req: Request, name: Text) : Result(Bool, Response)
  raw = try require_query_raw(req, name)
  case parse_bool_scalar(raw)
    Some(v): Ok(v)
    None: Error(validation_failure("/query/#{name}", "expected true or false"))
  end
end

fn require_query_text(req: Request, name: Text) : Result(Text, Response)
  require_query_raw(req, name)
end

fn optional_query_int(req: Request, name: Text) : Result(Option(Int), Response)
  case query_param(req, name)
    None: Ok(None)
    Some(raw):
      case raw.to_int
        Some(v): Ok(Some(v))
        None: Error(validation_failure("/query/#{name}", "expected integer"))
      end
  end
end

fn optional_query_byte(req: Request, name: Text) : Result(Option(Byte), Response)
  case query_param(req, name)
    None: Ok(None)
    Some(raw):
      case parse_byte_scalar(raw)
        Some(v): Ok(Some(v))
        None: Error(validation_failure("/query/#{name}", "expected byte 0..255"))
      end
  end
end

fn optional_query_bigint(req: Request, name: Text) : Result(Option(BigInt), Response)
  case query_param(req, name)
    None: Ok(None)
    Some(raw):
      case parse_big_scalar(raw)
        Some(v): Ok(Some(v))
        None: Error(validation_failure("/query/#{name}", "expected integer"))
      end
  end
end

fn optional_query_decimal(req: Request, name: Text) : Result(Option(Decimal), Response)
  case query_param(req, name)
    None: Ok(None)
    Some(raw):
      case raw.to_decimal
        Some(v): Ok(Some(v))
        None: Error(validation_failure("/query/#{name}", "expected decimal"))
      end
  end
end

fn optional_query_float(req: Request, name: Text) : Result(Option(Float), Response)
  case query_param(req, name)
    None: Ok(None)
    Some(raw):
      case raw.to_float
        Some(v):
          if v.nan? or v.infinite?
            Error(validation_failure("/query/#{name}", "expected finite float"))
          else
            Ok(Some(v))
          end
        None: Error(validation_failure("/query/#{name}", "expected float"))
      end
  end
end

fn optional_query_bool(req: Request, name: Text) : Result(Option(Bool), Response)
  case query_param(req, name)
    None: Ok(None)
    Some(raw):
      case parse_bool_scalar(raw)
        Some(v): Ok(Some(v))
        None: Error(validation_failure("/query/#{name}", "expected true or false"))
      end
  end
end

fn optional_query_text(req: Request, name: Text) : Result(Option(Text), Response)
  Ok(query_param(req, name))
end

## Formats a `Json.JsonDecodeError` on an incoming request body as a `422` validation response.
fn body_decode_error(err: Json.JsonDecodeError) : Response
  case err
    Json.NotUtf8: validation_failure("/body", "request body is not valid UTF-8")
    Json.Syntax(pe): validation_failure("/body", "#{pe}")
    Json.Mismatch(path: ptr, message: reason):
      p = if ptr.empty?: "/body" else: "/body#{ptr}"
      validation_failure(p, reason)
  end
end

## Formats a domain error variant's `Json` encoding with its mapped HTTP status code.
fn domain_error_response(status: Int, err_json: Json) : Response
  case err_json
    Json.String(code): encoded_json_response(status, Json.Object({"error": Json.String(code)}))
    Json.Object(entries):
      if entries.size == 1
        pair = entries.entries.first or ("error", Json.Null)
        encoded_json_response(status, Json.Object({"error": Json.String(pair.0), "detail": pair.1}))
      else
        encoded_json_response(status, Json.Object({"error": err_json}))
      end
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.Array(_): encoded_json_response(
      status,
      Json.Object({"error": err_json})
    )
  end
end

fn not_found_response() : Response
  encoded_json_response(404, Json.Object({"error": Json.String("not_found")}))
end

fn method_not_allowed_response() : Response
  encoded_json_response(405, Json.Object({"error": Json.String("method_not_allowed")}))
end

fn sqlite_open_error_response(err: SqliteError) : Response
  encoded_json_response(
    500,
    Json.Object({"error": Json.String("database_error"), "reason": Json.String("#{err}")})
  )
end

fn cors_preflight(origin: Text) : Response
  Response(
    status: 204,
    headers: [
      ("Access-Control-Allow-Origin", origin),
      ("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS"),
      ("Access-Control-Allow-Headers", "Authorization, Content-Type")
    ],
    body: "".bytes
  )
end

fn apply_cors(res: Response, origin: Text) : Response
  return res if origin.empty?
  with_header(res, "Access-Control-Allow-Origin", origin)
end

## Interactive HTML OpenAPI explorer served at `GET /docs`.
fn openapi_docs_html(title: Text) : Response
  page = """
    <!doctype html>
    <html lang="en">
    <head>
      <meta charset="utf-8">
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <title>#{title} — OpenAPI</title>
      <style>
        body { font-family: system-ui, sans-serif; max-width: 920px; margin: 2rem auto; padding: 0 1rem; color: #111; }
        pre { background: #f6f8fa; padding: 1rem; border-radius: 8px; overflow-x: auto; }
      </style>
    </head>
    <body>
      <h1>#{title} API</h1>
      <p>OpenAPI 3.1 specification: <a href="/openapi.json"><code>/openapi.json</code></a></p>
      <pre id="spec">Loading /openapi.json...</pre>
      <script>
        fetch('/openapi.json').then(r => r.text()).then(t => { document.getElementById('spec').textContent = t; });
      </script>
    </body>
    </html>
    """
  html_response(200, page)
end

test "request and response builders, path matching and validation helpers"
  req = post("/users?dry_run=true").with_bearer("tok123").with_json({"name": "Ada"})
  assert req.method == "POST"
  assert req.path == "/users"
  assert req.query_param("dry_run") == Some("true")
  assert req.bearer_token == Some("tok123")
  assert req.header("content-type") == Some("application/json")
  assert req.text == Some("{\"name\":\"Ada\"}")

  assert match_path("/users/:id/items/:slug", "/users/99/items/my%20item") ==
    Some(["99", "my item"])
  assert parse_path_int("99", "id") == Ok(99)
  assert parse_path_int("bad", "id") is Error(bad_id)
  assert bad_id.status == 422
  assert optional_query_bool(req, "dry_run") == Ok(Some(true))
  assert optional_query_int(req, "missing") == Ok(None)

  res = json_response(201, {"id": 1})
  assert res.ok?
  assert res.status == 201
  assert res.header("Content-Type") == Some("application/json")
  assert res.text == "{\"id\":1}"

  sse = sse_response([(Some("token"), "hello"), (None, "done")])
  assert sse.status == 200
  assert sse.text == "event: token\ndata: hello\n\ndata: done\n\n"
end

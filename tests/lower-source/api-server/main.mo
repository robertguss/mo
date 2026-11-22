module Main
use Api
use Json

struct Principal
  login: Text
end

struct CreateNote
  title: Text
  pinned: Bool
end

struct NoteView
  id: Int
  author: Text
  title: Text
  pinned: Bool
  tag: Text
end

enum NoteError
  Unauthorized
  NoteMissing(id: Int)
  EmptyTitle
end

api
  port 8080
  cors "https://app.example"
  static "/assets" from "public"
  auth Principal via authenticate
  get "/health" health
  get "/whoami" whoami
  scope "/notes"
    post "" create_note
    get "/:id" read_note
    delete "/:id" remove_note
  end
  get "/stream" stream_events
  status Unauthorized = 401
  status NoteMissing(_) = 404
  status EmptyTitle = 422
end

fn authenticate(req: Api.Request, env: Env) : Result(Principal, NoteError)
  expected = env.get("API_TOKEN") or "secret"
  token = req.bearer_token or ""
  return Error(Unauthorized) if token != expected
  Ok(Principal(login: "ada"))
end

## Reports server health.
fn health() : Map(Text, Text)
  {"status": "ok"}
end

## Reports optional principal, preserves repeated Set-Cookie headers, or redirects.
fn whoami(req: Api.Request, viewer: Option(Principal)) : Api.Response
  return Api.redirect_see_other("/notes") if req.query_param("go") == Some("1")
  who = case viewer
    Some(u): u.login
    None: "guest"
  end
  tags = req.headers_named("X-Tag")
  body = if tags.empty?: "{\"viewer\":\"#{who}\"}" else: "#{who}:#{tags.join(",")}"
  r0 = Api.text_response(200, body)
  r1 = r0.add_header("Set-Cookie", "sid=a; HttpOnly")
  r1.add_header("Set-Cookie", "csrf=b")
end

## Creates a note with strict JSON body decoding.
fn create_note(user: Principal, input: CreateNote, tag: Option(Text)) : Result(NoteView, NoteError)
  return Error(EmptyTitle) if input.title.trim.empty?
  Ok(
    NoteView(
      id: 1,
      author: user.login,
      title: input.title,
      pinned: input.pinned,
      tag: tag or "general"
    )
  )
end

## Reads a note by numeric path parameter and required query parameter.
fn read_note(id: Int, format: Text) : Result(NoteView, NoteError)
  return Error(NoteMissing(id: id)) if id <= 0
  Ok(NoteView(id: id, author: "public", title: "note-#{id}-#{format}", pinned: false, tag: "doc"))
end

## Deletes a note when authenticated.
fn remove_note(_user: Principal, id: Int) : Result(Map(Text, Int), NoteError)
  return Error(NoteMissing(id: id)) if id == 99
  Ok({"deleted": id})
end

## Streams Server-Sent Events via a raw Response.
fn stream_events(req: Api.Request) : Api.Response
  topic = req.header("X-Topic") or "all"
  Api.sse_response([(Some("ready"), "{\"topic\":\"#{topic}\"}")])
end

fn main() : Int
  0
end

test "api health and cors"
  env = Env.of({"API_TOKEN": "topsecret"})
  r_health = ApiTest.send(Main, Api.get("/health"), env)
  assert r_health.status == 200
  assert r_health.header("Access-Control-Allow-Origin") == Some("https://app.example")
  assert r_health.text == "{\"status\":\"ok\"}"
end

test "api preflight and bearer auth"
  env = Env.of({"API_TOKEN": "topsecret"})
  r_opts = ApiTest.send(Main, Api.request("OPTIONS", "/notes"), env)
  assert r_opts.status == 204
  assert r_opts.header("Access-Control-Allow-Origin") == Some("https://app.example")

  r_unauth = ApiTest.send(
    Main,
    Api.post("/notes").with_json(CreateNote(title: "hi", pinned: true)),
    env
  )
  assert r_unauth.status == 401
end

test "api strict json rejection"
  env = Env.of({"API_TOKEN": "topsecret"})
  r_strict = ApiTest.send(
    Main,
    Api.post("/notes").with_bearer("topsecret").with_bytes(
      "{\"title\":\"hi\",\"pinned\":true,\"extra\":1}".bytes,
      "application/json"
    ),
    env
  )
  assert r_strict.status == 422
end

test "api body decode and optional query"
  env = Env.of({"API_TOKEN": "topsecret"})
  r_ok = ApiTest.send(
    Main,
    Api.post("/notes?tag=work").with_bearer("topsecret").with_json(
      CreateNote(title: "ship it", pinned: true)
    ),
    env
  )
  assert r_ok.status == 200
  assert Json.decode_strict[NoteView](r_ok.text) ==
    Ok(NoteView(id: 1, author: "ada", title: "ship it", pinned: true, tag: "work"))
end

test "api path params, query params and status mapping"
  env = Env.of({"API_TOKEN": "topsecret"})
  r_read = ApiTest.send(Main, Api.get("/notes/7?format=md"), env)
  assert r_read.status == 200
  assert r_read.text.contains?("note-7-md")

  r_nf = ApiTest.send(Main, Api.get("/notes/0?format=md"), env)
  assert r_nf.status == 404

  r_405 = ApiTest.send(Main, Api.post("/health"), env)
  assert r_405.status == 405
end

test "api sse response"
  env = Env.of({"API_TOKEN": "topsecret"})
  r_sse = ApiTest.send(Main, Api.get("/stream").with_header("X-Topic", "builds"), env)
  assert r_sse.status == 200
  assert r_sse.header("Content-Type") == Some("text/event-stream")
  assert r_sse.text == "event: ready\ndata: {\"topic\":\"builds\"}\n\n"
end

test "api openapi 3.1 document"
  env = Env.of({"API_TOKEN": "topsecret"})
  r_spec = ApiTest.send(Main, Api.get("/openapi.json"), env)
  assert r_spec.status == 200
  spec_text = r_spec.text
  assert spec_text.contains?("\"openapi\":\"3.1.0\"")
  assert spec_text.contains?("\"/notes/{id}\"")
end

test "api static assets, optional auth, multi-headers and see-other redirect"
  env = Env.of({"API_TOKEN": "topsecret"})
  files = Files.in_memory({
    "public/app-a1b2c3d4.js": "console.log(1)",
    "public/index.html": "<h1>App</h1>",
    "secret.txt": "nope"
  })

  guest = ApiTest.send(Main, Api.get("/whoami"), env)
  assert guest.status == 200 and guest.text == "{\"viewer\":\"guest\"}"
  bad_tok = ApiTest.send(Main, Api.get("/whoami").with_bearer("wrong"), env)
  assert bad_tok.status == 200 and bad_tok.text == "{\"viewer\":\"guest\"}"
  authed = ApiTest.send(Main, Api.get("/whoami").with_bearer("topsecret"), env)
  assert authed.status == 200 and authed.text == "{\"viewer\":\"ada\"}"

  req_tags = Api.get("/whoami").add_header("X-Tag", "one").add_header("x-tag", "two")
  cookies = ApiTest.send(Main, req_tags, env)
  assert cookies.status == 200 and cookies.text == "guest:one,two"
  assert cookies.headers_named("set-cookie") == ["sid=a; HttpOnly", "csrf=b"]

  redir = ApiTest.send(Main, Api.get("/whoami?go=1"), env)
  assert redir.status == 303 and redir.header("Location") == Some("/notes")

  js = ApiTest.send(Main, Api.get("/assets/app-a1b2c3d4.js"), files.read_only, env)
  assert js.status == 200
  assert js.header("Content-Type") == Some("text/javascript; charset=utf-8")
  assert js.header("X-Content-Type-Options") == Some("nosniff")
  assert js.header("Cache-Control") == Some("public, max-age=31536000, immutable")
  etag = js.header("ETag") or ""
  assert not etag.empty?

  not_mod = ApiTest.send(
    Main,
    Api.get("/assets/app-a1b2c3d4.js").with_header("If-None-Match", etag),
    files.read_only,
    env
  )
  assert not_mod.status == 304 and not_mod.body.size == 0

  head_res = ApiTest.send(Main, Api.request("HEAD", "/assets/index.html"), files.read_only, env)
  assert head_res.status == 200 and head_res.body.size == 0
  assert head_res.header("Cache-Control") == Some("public, max-age=0, must-revalidate")

  trav = ApiTest.send(Main, Api.get("/assets/../secret.txt"), files.read_only, env)
  assert trav.status == 404
end

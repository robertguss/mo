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
  auth Principal via authenticate
  get "/health" health
  post "/notes" create_note
  get "/notes/:id" read_note
  delete "/notes/:id" remove_note
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
  assert r_spec.text.contains?("\"openapi\":\"3.1.0\"")
  assert r_spec.text.contains?("\"/notes/{id}\"")
end

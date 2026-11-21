## `mo-pocket` — Typed single-binary backend-in-a-box (`PocketBase` in Mo): SQLite user
## auth, collection CRUD with ownership rules, OpenAPI 3.1, and `RealtimeHub` SSE events.
module Main
use Api
use Json

struct ChangeEvent
  seq: Int
  collection: Text
  action: Text
  record_id: Int
  owner: Text
end

struct HubState
  seq: Int
  events: List(ChangeEvent)
end

struct PocketUser
  email: Text
  admin?: Bool
end

struct AuthCredentials
  email: Text
  secret: Text
end

struct SessionToken
  email: Text
  token: Text
end

struct RecordInput
  data: Json
end

struct StoredRecord
  id: Int
  collection: Text
  owner: Text
  data: Json
end

enum PocketError
  Unauthenticated
  ForbiddenAction
  RecordMissing(id: Int)
  EmailConflict(email: Text)
  InvalidCollection(name: Text)
  PocketDbFailed(message: Text)
end

## Validates a safe collection identifier (`snake_case` ASCII).
fn valid_collection?(name: Text) : Bool
  promise
    result implies not name.empty?
  end

  examples
    valid_collection?("tasks")
    valid_collection?("user_notes_2")
    not valid_collection?("../etc")
  end

  return false if name.empty? or name.byte_count > 64
  first = name.chars.first or '0'
  first.ascii_letter? and
    name.chars.all?(fn(c) c.ascii_letter? or c.ascii_digit? or c == '_' end) and
    name == name.ascii_lowercase
end

actor RealtimeHub
  state HubState = HubState(seq: 0, events: [])
  calls publish_change, events_since
  every 10.minutes trim_event_log

  promise
    after.seq >= before.seq
  end

  promise on publish_change
    after.seq >= before.seq
  end
end

fn publish_change(
  state: HubState,
  _now: Time,
  collection: Text,
  action: Text,
  record_id: Int,
  owner: Text
) : (HubState, ChangeEvent)
  next_seq = state.seq.saturating_add(1)
  ev = ChangeEvent(
    seq: next_seq,
    collection: collection,
    action: action,
    record_id: record_id,
    owner: owner
  )
  kept = if state.events.size >= 256: state.events.drop(1) else: state.events
  (HubState(seq: next_seq, events: kept.append(ev)), ev)
end

fn events_since(state: HubState, _now: Time, collection: Text, since_seq: Int) : List(ChangeEvent)
  state.events.filter(fn(e) e.collection == collection and e.seq > since_seq end)
end

fn trim_event_log(state: HubState, _now: Time) : HubState
  kept = if state.events.size > 128
    state.events.drop(state.events.size - 128)
  else
    state.events
  end
  HubState(seq: state.seq, events: kept)
end

api
  port 8105
  cors "*"
  sqlite db from "pocket.sqlite"
  actor hub: RealtimeHub
  auth PocketUser via verify_session

  post "/auth/register" post_register
  post "/auth/login" post_login
  post "/collections/:collection/records" post_create_record
  get "/collections/:collection/records" get_list_records
  get "/collections/:collection/records/:id" get_single_record
  delete "/collections/:collection/records/:id" delete_single_record
  get "/realtime/:collection" get_realtime_sse

  status Unauthenticated = 401
  status ForbiddenAction = 403
  status RecordMissing(_) = 404
  status EmailConflict(_) = 409
  status InvalidCollection(_) = 400
  status PocketDbFailed(_) = 500
end

fn init_pocket_db(db: Sqlite.Database) : Result(Unit, PocketError)
  s1 = [
    "CREATE TABLE IF NOT EXISTS users (",
    "  email TEXT PRIMARY KEY,",
    "  secret TEXT NOT NULL,",
    "  is_admin INT NOT NULL,",
    "  token TEXT NOT NULL",
    ")"
  ].join("")
  s2 = [
    "CREATE TABLE IF NOT EXISTS records (",
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,",
    "  collection TEXT NOT NULL,",
    "  owner TEXT NOT NULL,",
    "  payload TEXT NOT NULL",
    ")"
  ].join("")
  try db.run(s1, []).map_error(fn(e) PocketDbFailed(message: "#{e}") end)
  db.run(s2, []).map_error(fn(e) PocketDbFailed(message: "#{e}") end)
end

fn cell_str(row: List(Sqlite.Value), idx: Int) : Text
  if row.get(idx) is Some(Sqlite.String(s)): s else: ""
end

fn cell_int(row: List(Sqlite.Value), idx: Int) : Int
  if row.get(idx) is Some(Sqlite.Integer(n)): n else: 0
end

fn verify_session(req: Api.Request, db: Sqlite.Database) : Result(PocketUser, PocketError)
  try init_pocket_db(db)
  tok = try req.bearer_token.or_fail(Unauthenticated)
  rows = try db.query("SELECT email, is_admin FROM users WHERE token = ?", [Sqlite.String(tok)])
    .map_error(fn(e) PocketDbFailed(message: "#{e}") end)
  row = try rows.rows.first.or_fail(Unauthenticated)
  Ok(PocketUser(email: cell_str(row, 0), admin?: cell_int(row, 1) == 1))
end

## Registers a new user account (the first user becomes admin) and issues a session token.
fn post_register(db: Sqlite.Database, creds: AuthCredentials) : Result(SessionToken, PocketError)
  try init_pocket_db(db)
  email = creds.email.trim.ascii_lowercase
  return Error(Unauthenticated) if email.empty? or creds.secret.empty?
  existing = try db.query("SELECT email FROM users WHERE email = ?", [Sqlite.String(email)])
    .map_error(fn(e) PocketDbFailed(message: "#{e}") end)
  return Error(EmailConflict(email: email)) if not existing.rows.empty?

  count_rows = try db.query("SELECT COUNT(*) FROM users", []).map_error(
    fn(e) PocketDbFailed(message: "#{e}") end
  )
  first_user = cell_int(count_rows.rows.first or [], 0) == 0
  tok = "pkt_#{email}_#{creds.secret.byte_count}"
  try db.run(
    "INSERT INTO users (email, secret, is_admin, token) VALUES (?, ?, ?, ?)",
    [
      Sqlite.String(email),
      Sqlite.String(creds.secret),
      Sqlite.Integer(if first_user: 1 else: 0),
      Sqlite.String(tok)
    ]
  ).map_error(fn(e) PocketDbFailed(message: "#{e}") end)
  Ok(SessionToken(email: email, token: tok))
end

## Authenticates credentials and returns the user's session token.
fn post_login(db: Sqlite.Database, creds: AuthCredentials) : Result(SessionToken, PocketError)
  try init_pocket_db(db)
  email = creds.email.trim.ascii_lowercase
  rows = try db.query(
    "SELECT token FROM users WHERE email = ? AND secret = ?",
    [Sqlite.String(email), Sqlite.String(creds.secret)]
  ).map_error(fn(e) PocketDbFailed(message: "#{e}") end)
  row = try rows.rows.first.or_fail(Unauthenticated)
  Ok(SessionToken(email: email, token: cell_str(row, 0)))
end

## Creates a typed JSON record in a collection and broadcasts an SSE event via RealtimeHub.
fn post_create_record(
  user: PocketUser,
  db: Sqlite.Database,
  hub: RealtimeHub,
  collection: Text,
  body: RecordInput
) : Result(StoredRecord, PocketError)
  return Error(InvalidCollection(name: collection)) if not valid_collection?(collection)
  try init_pocket_db(db)
  try db.run(
    "INSERT INTO records (collection, owner, payload) VALUES (?, ?, ?)",
    [Sqlite.String(collection), Sqlite.String(user.email), Sqlite.String(body.data.to_text)]
  ).map_error(fn(e) PocketDbFailed(message: "#{e}") end)
  id_rows = try db.query("SELECT last_insert_rowid()", []).map_error(
    fn(e) PocketDbFailed(message: "#{e}") end
  )
  rec_id = cell_int(id_rows.rows.first or [], 0)
  _ev = hub.publish_change(collection, "created", rec_id, user.email)
  Ok(StoredRecord(id: rec_id, collection: collection, owner: user.email, data: body.data))
end

## Lists all records in a collection.
fn get_list_records(db: Sqlite.Database, collection: Text) : Result(List(StoredRecord), PocketError)
  return Error(InvalidCollection(name: collection)) if not valid_collection?(collection)
  try init_pocket_db(db)
  rows = try db.query(
    "SELECT id, owner, payload FROM records WHERE collection = ? ORDER BY id ASC",
    [Sqlite.String(collection)]
  ).map_error(fn(e) PocketDbFailed(message: "#{e}") end)
  Ok(
    rows.rows.map(fn(r)
      parsed = case Json.parse(cell_str(r, 2))
        Ok(j): j
        Error(_): Json.Null
      end
      StoredRecord(id: cell_int(r, 0), collection: collection, owner: cell_str(r, 1), data: parsed)
    end)
  )
end

## Fetches a single record by ID.
fn get_single_record(db: Sqlite.Database, collection: Text, id: Int) : Result(
  StoredRecord,
  PocketError
)
  return Error(InvalidCollection(name: collection)) if not valid_collection?(collection)
  try init_pocket_db(db)
  rows = try db.query(
    "SELECT owner, payload FROM records WHERE collection = ? AND id = ?",
    [Sqlite.String(collection), Sqlite.Integer(id)]
  ).map_error(fn(e) PocketDbFailed(message: "#{e}") end)
  row = try rows.rows.first.or_fail(RecordMissing(id: id))
  parsed = case Json.parse(cell_str(row, 1))
    Ok(j): j
    Error(_): Json.Null
  end
  Ok(StoredRecord(id: id, collection: collection, owner: cell_str(row, 0), data: parsed))
end

## Deletes a record (permitted only for the record's owner or an admin).
fn delete_single_record(
  user: PocketUser,
  db: Sqlite.Database,
  hub: RealtimeHub,
  collection: Text,
  id: Int
) : Result(Unit, PocketError)
  rec = try get_single_record(db, collection, id)
  return Error(ForbiddenAction) if not user.admin? and rec.owner != user.email
  try db.run(
    "DELETE FROM records WHERE collection = ? AND id = ?",
    [Sqlite.String(collection), Sqlite.Integer(id)]
  ).map_error(fn(e) PocketDbFailed(message: "#{e}") end)
  _ev = hub.publish_change(collection, "deleted", id, user.email)
  Ok(Unit)
end

## Streams collection change events after `since` as Server-Sent Events (`text/event-stream`).
fn get_realtime_sse(hub: RealtimeHub, collection: Text, since: Option(Int)) : Result(
  Api.Response,
  PocketError
)
  return Error(InvalidCollection(name: collection)) if not valid_collection?(collection)
  events = hub.events_since(collection, since or 0)
  sse_items = events.map(fn(ev) (Some(ev.action), Json.encode(ev)) end)
  Ok(Api.sse_response(sse_items))
end

test "auth, ownership ACLs, record CRUD, OpenAPI 3.1, and RealtimeHub SSE feed"
  sqlite = Sqlite.in_memory()
  hub = Actor.start(RealtimeHub)

  r_admin = ApiTest.send(
    Main,
    Api.post("/auth/register").with_json(AuthCredentials(email: "ada@mo.dev", secret: "pw-admin")),
    sqlite,
    hub
  )
  assert r_admin.status == 200
  assert Json.decode[SessionToken](r_admin.text) is Ok(admin_tok)

  r_member = ApiTest.send(
    Main,
    Api.post("/auth/register").with_json(AuthCredentials(email: "bob@mo.dev", secret: "pw-bob")),
    sqlite,
    hub
  )
  assert Json.decode[SessionToken](r_member.text) is Ok(bob_tok)

  dup = ApiTest.send(
    Main,
    Api.post("/auth/register").with_json(AuthCredentials(email: "bob@mo.dev", secret: "pw-bob")),
    sqlite,
    hub
  )
  assert dup.status == 409

  unauth = ApiTest.send(
    Main,
    Api.post("/collections/tasks/records").with_json(
      RecordInput(data: Json.Object({"title": Json.String("Ship Mo")}))
    ),
    sqlite,
    hub
  )
  assert unauth.status == 401

  created = ApiTest.send(
    Main,
    Api.post("/collections/tasks/records").with_bearer(admin_tok.token).with_json(
      RecordInput(data: Json.Object({"title": Json.String("Ship Mo")}))
    ),
    sqlite,
    hub
  )
  assert created.status == 200
  assert Json.decode[StoredRecord](created.text) is Ok(rec1)
  assert rec1.id == 1 and rec1.owner == "ada@mo.dev"

  forbidden_del = ApiTest.send(
    Main,
    Api.delete("/collections/tasks/records/1").with_bearer(bob_tok.token),
    sqlite,
    hub
  )
  assert forbidden_del.status == 403

  ok_del = ApiTest.send(
    Main,
    Api.delete("/collections/tasks/records/1").with_bearer(admin_tok.token),
    sqlite,
    hub
  )
  assert ok_del.status == 204

  sse = ApiTest.send(Main, Api.get("/realtime/tasks").with_query("since", "0"), sqlite, hub)
  assert sse.status == 200
  assert sse.text.contains?("event: created")
  assert sse.text.contains?("event: deleted")

  openapi = ApiTest.send(Main, Api.get("/openapi.json"), sqlite, hub)
  assert openapi.status == 200
  assert openapi.text.contains?("\"/collections/{collection}/records\"")
end

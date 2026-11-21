module Main
use Api
use Json

struct Principal
  login: Text
end

struct CreateTask
  title: Text
end

struct TaskRow
  id: Int
  title: Text
  owner: Text
end

enum ApiError
  Unauthorized
  TaskMissing(id: Int)
  DbFailed(message: Text)
end

api
  port 8080
  cors "*"
  sqlite db from "tasks.db"
  auth Principal via authenticate
  post "/tasks" create_task
  get "/tasks/:id" get_task
  get "/tasks" list_tasks
  status Unauthorized = 401
  status TaskMissing(_) = 404
  status DbFailed(_) = 500
end

fn map_sql(res: Result(T, SqliteError)) : Result(T, ApiError)
  res.map_error(fn(e) DbFailed(message: "#{e}") end)
end

fn init_schema(db: Sqlite.Database) : Result(Unit, ApiError)
  map_sql(
    db.run(
      "CREATE TABLE IF NOT EXISTS tasks(id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, owner TEXT NOT NULL)",
      []
    )
  )
end

fn authenticate(req: Api.Request, env: Env) : Result(Principal, ApiError)
  secret = env.get("API_SECRET") or "dev-secret"
  token = req.bearer_token or ""
  return Error(Unauthorized) if token != secret
  Ok(Principal(login: "grace"))
end

## Inserts a task into SQLite and returns the created record.
fn create_task(user: Principal, db: Sqlite.Database, body: CreateTask) : Result(TaskRow, ApiError)
  try init_schema(db)
  try map_sql(
    db.run(
      "INSERT INTO tasks(title, owner) VALUES (?, ?)",
      [Sqlite.String(body.title), Sqlite.String(user.login)]
    )
  )
  rows = try map_sql(db.query("SELECT last_insert_rowid()", []))
  case rows.rows
    [[Sqlite.Integer(id)]]: Ok(TaskRow(id: id, title: body.title, owner: user.login))
    _: Error(DbFailed(message: "missing rowid"))
  end
end

## Looks up a single task by id.
fn get_task(db: Sqlite.Database, id: Int) : Result(TaskRow, ApiError)
  try init_schema(db)
  rows = try map_sql(
    db.query("SELECT id, title, owner FROM tasks WHERE id = ?", [Sqlite.Integer(id)])
  )
  case rows.rows
    [[Sqlite.Integer(rid), Sqlite.String(title), Sqlite.String(owner)]]: Ok(
      TaskRow(id: rid, title: title, owner: owner)
    )
    []: Error(TaskMissing(id: id))
    _: Error(DbFailed(message: "unexpected row shape"))
  end
end

## Lists tasks with an optional limit query parameter.
fn list_tasks(db: Sqlite.Database, limit: Option(Int)) : Result(List(TaskRow), ApiError)
  try init_schema(db)
  cap = limit or 50
  rows = try map_sql(
    db.query("SELECT id, title, owner FROM tasks ORDER BY id ASC LIMIT ?", [Sqlite.Integer(cap)])
  )
  var out = []
  for row in rows.rows
    case row
      [Sqlite.Integer(rid), Sqlite.String(title), Sqlite.String(owner)]: out = out.append(
        TaskRow(id: rid, title: title, owner: owner)
      )
      _: return Error(DbFailed(message: "unexpected row"))
    end
  end
  Ok(out)
end

test "sqlite-backed api server end to end via ApiTest.send"
  sqlite = Sqlite.in_memory()
  env = Env.of({"API_SECRET": "k1"})

  created = ApiTest.send(
    Main,
    Api.post("/tasks").with_bearer("k1").with_json(CreateTask(title: "write docs")),
    sqlite,
    env
  )
  assert created.status == 200
  assert Json.decode_strict[TaskRow](created.text) ==
    Ok(TaskRow(id: 1, title: "write docs", owner: "grace"))

  fetched = ApiTest.send(Main, Api.get("/tasks/1"), sqlite, env)
  assert fetched.status == 200
  assert Json.decode_strict[TaskRow](fetched.text) ==
    Ok(TaskRow(id: 1, title: "write docs", owner: "grace"))

  missing = ApiTest.send(Main, Api.get("/tasks/99"), sqlite, env)
  assert missing.status == 404

  listed = ApiTest.send(Main, Api.get("/tasks?limit=5"), sqlite, env)
  assert listed.status == 200
  assert Json.decode_strict[List(TaskRow)](listed.text) ==
    Ok([TaskRow(id: 1, title: "write docs", owner: "grace")])

  docs = ApiTest.send(Main, Api.get("/docs"), sqlite, env)
  assert docs.status == 200
  assert docs.text.contains?("/openapi.json")
end

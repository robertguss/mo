## `mo-pipe` — Single-binary scheduled API sync and ETL daemon with `PipeScheduler` `actor`,
## SQLite watermark cursor tracking, schema-drift alerts, and status `api`.
module Main
use Api
use Http
use Json

struct PipelineSpec
  name: Text
  endpoint: Text
  required_fields: List(Text)
end

struct PipelineStatus
  name: Text
  endpoint: Text
  required_fields: List(Text)
  due?: Bool
  runs: Int
  records_synced: Int
  drift_alerts: Int
end

struct SchedulerState
  pipelines: Map(Text, PipelineStatus)
  ticks: Int
end

struct SyncOutcome
  pipeline: Text
  fetched: Int
  new_cursor: Option(Text)
  drift_detected?: Bool
  missing_fields: List(Text)
end

struct DriftAlert
  pipeline: Text
  record_id: Text
  missing_field: Text
end

enum PipeError
  InvalidPipeline(reason: Text)
  UnknownPipeline(name: Text)
  UpstreamSyncFailed(message: Text)
  PipeDbError(message: Text)
end

actor PipeScheduler
  state SchedulerState = SchedulerState(pipelines: {}, ticks: 0)
  calls register_spec, record_run, all_pipelines
  every 30.seconds mark_all_due

  promise
    after.ticks >= 0
  end

  promise on record_run
    after.pipelines.size == before.pipelines.size
  end
end

fn register_spec(state: SchedulerState, _now: Time, spec: PipelineSpec) : Result(
  (SchedulerState, PipelineStatus),
  PipeError
)
  trimmed = spec.name.trim
  return Error(InvalidPipeline(reason: "name cannot be empty")) if trimmed.empty?
  return Error(InvalidPipeline(reason: "endpoint must start with http")) if not (
    spec.endpoint.starts_with?("http://") or spec.endpoint.starts_with?("https://")
  )
  st = PipelineStatus(
    name: trimmed,
    endpoint: spec.endpoint,
    required_fields: spec.required_fields,
    due?: true,
    runs: 0,
    records_synced: 0,
    drift_alerts: 0
  )
  next = SchedulerState(pipelines: state.pipelines.set(trimmed, st), ticks: state.ticks)
  Ok((next, st))
end

fn record_run(state: SchedulerState, _now: Time, name: Text, fetched: Int, drifts: Int) : Result(
  (SchedulerState, PipelineStatus),
  PipeError
)
  prev = try state.pipelines.get(name).or_fail(UnknownPipeline(name: name))
  safe_f = if fetched > 0: fetched else: 0
  safe_d = if drifts > 0: drifts else: 0
  updated = PipelineStatus(
    name: prev.name,
    endpoint: prev.endpoint,
    required_fields: prev.required_fields,
    due?: false,
    runs: prev.runs.saturating_add(1),
    records_synced: prev.records_synced.saturating_add(safe_f),
    drift_alerts: prev.drift_alerts.saturating_add(safe_d)
  )
  next = SchedulerState(pipelines: state.pipelines.set(name, updated), ticks: state.ticks)
  Ok((next, updated))
end

fn all_pipelines(state: SchedulerState, _now: Time) : List(PipelineStatus)
  state.pipelines.values
end

fn mark_all_due(state: SchedulerState, _now: Time) : SchedulerState
  var updated = {}
  for (k, v) in state.pipelines.entries
    updated = updated.set(
      k,
      PipelineStatus(
        name: v.name,
        endpoint: v.endpoint,
        required_fields: v.required_fields,
        due?: true,
        runs: v.runs,
        records_synced: v.records_synced,
        drift_alerts: v.drift_alerts
      )
    )
  end
  SchedulerState(pipelines: updated, ticks: state.ticks.saturating_add(1))
end

api
  port 8102
  sqlite db from "pipe.sqlite"
  actor scheduler: PipeScheduler

  post "/pipelines" post_pipeline
  get "/pipelines" get_pipelines
  post "/pipelines/:name/run" post_run_pipeline
  get "/alerts" get_alerts

  status InvalidPipeline(_) = 400
  status UnknownPipeline(_) = 404
  status UpstreamSyncFailed(_) = 502
  status PipeDbError(_) = 500
end

fn init_pipe_db(db: Sqlite.Database) : Result(Unit, PipeError)
  s1 = [
    "CREATE TABLE IF NOT EXISTS watermarks (",
    "  pipeline TEXT PRIMARY KEY,",
    "  cursor TEXT NOT NULL",
    ")"
  ].join("")
  s2 = [
    "CREATE TABLE IF NOT EXISTS synced_records (",
    "  pipeline TEXT NOT NULL,",
    "  record_id TEXT NOT NULL,",
    "  payload TEXT NOT NULL,",
    "  PRIMARY KEY (pipeline, record_id)",
    ")"
  ].join("")
  s3 = [
    "CREATE TABLE IF NOT EXISTS drift_alerts (",
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,",
    "  pipeline TEXT NOT NULL,",
    "  record_id TEXT NOT NULL,",
    "  missing_field TEXT NOT NULL",
    ")"
  ].join("")
  try db.run(s1, []).map_error(fn(e) PipeDbError(message: "#{e}") end)
  try db.run(s2, []).map_error(fn(e) PipeDbError(message: "#{e}") end)
  db.run(s3, []).map_error(fn(e) PipeDbError(message: "#{e}") end)
end

fn cell_str(row: List(Sqlite.Value), idx: Int) : Text
  if row.get(idx) is Some(Sqlite.String(s)): s else: ""
end

fn read_watermark(db: Sqlite.Database, name: Text) : Result(Option(Text), PipeError)
  rows = try db.query("SELECT cursor FROM watermarks WHERE pipeline = ?", [Sqlite.String(name)])
    .map_error(fn(e) PipeDbError(message: "#{e}") end)
  case rows.rows.first
    Some(r):
      c = cell_str(r, 0)
      if c.empty?: Ok(None) else: Ok(Some(c))
    None: Ok(None)
  end
end

fn save_watermark(db: Sqlite.Database, name: Text, cursor: Text) : Result(Unit, PipeError)
  db.run(
    [
      "INSERT INTO watermarks (pipeline, cursor) VALUES (?, ?) ",
      "ON CONFLICT(pipeline) DO UPDATE SET cursor = excluded.cursor"
    ].join(""),
    [Sqlite.String(name), Sqlite.String(cursor)]
  ).map_error(fn(e) PipeDbError(message: "#{e}") end)
end

## Registers a scheduled ETL pipeline and its expected schema fields.
fn post_pipeline(scheduler: PipeScheduler, spec: PipelineSpec) : Result(PipelineStatus, PipeError)
  scheduler.register_spec(spec)
end

## Lists all registered pipelines and their scheduler/watermark health.
fn get_pipelines(scheduler: PipeScheduler) : List(PipelineStatus)
  scheduler.all_pipelines
end

## Executes one incremental watermark sync cycle and records schema-drift alerts.
fn post_run_pipeline(scheduler: PipeScheduler, db: Sqlite.Database, net: Network, name: Text) : Result(
  SyncOutcome,
  PipeError
)
  try init_pipe_db(db)
  spec = try scheduler.all_pipelines.find(fn(p) p.name == name end).or_fail(
    UnknownPipeline(name: name)
  )

  cursor = try read_watermark(db, name)
  base_req = Http.get(spec.endpoint).header("Accept", "application/json")
  req = case cursor
    Some(c): base_req.query("since", c)
    None: base_req
  end
  resp = try req.send(net).map_error(fn(e) UpstreamSyncFailed(message: "#{e}") end)
  return Error(UpstreamSyncFailed(message: "HTTP #{resp.status}")) if not resp.ok?
  doc = try resp.json.map_error(fn(e) UpstreamSyncFailed(message: "#{e}") end)

  items = doc.list_at("items") or []
  next_cur = doc.text_at("next_cursor")

  var missing_accum = []
  var drift_count = 0
  for (idx, item) in items.enumerate
    rec_id = item.text_at("id") or "row_#{idx}"
    try db.run(
      [
        "INSERT INTO synced_records (pipeline, record_id, payload) VALUES (?, ?, ?) ",
        "ON CONFLICT(pipeline, record_id) DO UPDATE SET payload = excluded.payload"
      ].join(""),
      [Sqlite.String(name), Sqlite.String(rec_id), Sqlite.String(item.to_text)]
    ).map_error(fn(e) PipeDbError(message: "#{e}") end)

    for req_field in spec.required_fields
      if item.get(req_field) == None or item.get(req_field) == Some(Json.Null)
        drift_count += 1
        if not missing_accum.contains?(req_field)
          missing_accum = missing_accum.append(req_field)
        end
        try db.run(
          "INSERT INTO drift_alerts (pipeline, record_id, missing_field) VALUES (?, ?, ?)",
          [Sqlite.String(name), Sqlite.String(rec_id), Sqlite.String(req_field)]
        ).map_error(fn(e) PipeDbError(message: "#{e}") end)
      end
    end
  end

  case next_cur
    Some(c) if not c.empty?: try save_watermark(db, name, c)
    Some(_) | None: Unit
  end

  _updated = try scheduler.record_run(name, items.size, drift_count)
  Ok(
    SyncOutcome(
      pipeline: name,
      fetched: items.size,
      new_cursor: next_cur,
      drift_detected?: drift_count > 0,
      missing_fields: missing_accum
    )
  )
end

## Returns recorded schema-drift alerts from SQLite.
fn get_alerts(db: Sqlite.Database) : Result(List(DriftAlert), PipeError)
  try init_pipe_db(db)
  rows = try db.query(
    "SELECT pipeline, record_id, missing_field FROM drift_alerts ORDER BY id ASC",
    []
  ).map_error(fn(e) PipeDbError(message: "#{e}") end)
  Ok(
    rows.rows.map(fn(r)
      DriftAlert(pipeline: cell_str(r, 0), record_id: cell_str(r, 1), missing_field: cell_str(r, 2))
    end)
  )
end

test "watermark cursor progression, timer due scheduling, and schema-drift alerting"
  sqlite = Sqlite.in_memory()
  sched = Actor.start(PipeScheduler)
  endpoint = "https://crm.example.com/events"

  reg = ApiTest.send(
    Main,
    Api.post("/pipelines").with_json(
      PipelineSpec(name: "contacts", endpoint: endpoint, required_fields: ["id", "email"])
    ),
    sqlite,
    sched
  )
  assert reg.status == 200

  r1 = Http.get(endpoint).header("Accept", "application/json")
  r2 = Http.get(endpoint).header("Accept", "application/json").query("since", "cur_100")
  b1 = "{\"items\":[{\"id\":\"u1\",\"email\":\"ada@mo.dev\"}],\"next_cursor\":\"cur_100\"}"
  b2 = "{\"items\":[{\"id\":\"u2\",\"email\":null}],\"next_cursor\":\"cur_101\"}"
  net = Network.fake(Http.replies([(r1, Http.reply(200, b1)), (r2, Http.reply(200, b2))]))

  run1 = ApiTest.send(Main, Api.post("/pipelines/contacts/run"), sqlite, net, sched)
  assert run1.status == 200
  assert Json.decode[SyncOutcome](run1.text) ==
    Ok(
      SyncOutcome(
        pipeline: "contacts",
        fetched: 1,
        new_cursor: Some("cur_100"),
        drift_detected?: false,
        missing_fields: []
      )
    )

  Actor.advance(sched, 30.seconds)
  list1 = ApiTest.send(Main, Api.get("/pipelines"), sqlite, sched)
  assert Json.decode[List(PipelineStatus)](list1.text) is Ok([st1])
  assert st1.due? and st1.runs == 1

  run2 = ApiTest.send(Main, Api.post("/pipelines/contacts/run"), sqlite, net, sched)
  assert run2.status == 200
  assert Json.decode[SyncOutcome](run2.text) ==
    Ok(
      SyncOutcome(
        pipeline: "contacts",
        fetched: 1,
        new_cursor: Some("cur_101"),
        drift_detected?: true,
        missing_fields: ["email"]
      )
    )

  alerts_resp = ApiTest.send(Main, Api.get("/alerts"), sqlite, sched)
  assert Json.decode[List(DriftAlert)](alerts_resp.text) ==
    Ok([DriftAlert(pipeline: "contacts", record_id: "u2", missing_field: "email")])
end

## `mo-vcr` — Recording HTTPS proxy, deterministic cassette replay server, chaos injection
## `actor`, and SQLite payload drift inspector.
module Main
use Api
use Http
use Json

enum VcrMode
  Record
  Replay
end

struct ChaosState
  mode: VcrMode
  cassette: Text
  failure_every: Int
  total_requests: Int
  injected_429s: Int
end

struct ChaosDecision
  mode: VcrMode
  cassette: Text
  inject_429?: Bool
end

struct VcrStats
  mode: VcrMode
  cassette: Text
  failure_every: Int
  total_requests: Int
  injected_429s: Int
end

struct ModeConfig
  mode: VcrMode
  cassette: Text
  failure_every: Int
end

struct ProxyRequest
  method: Text
  url: Text
  body: Option(Text)
end

struct ProxyResult
  source: Text
  upstream_status: Int
  body: Text
end

struct DriftReport
  cassette: Text
  url: Text
  snapshots: Int
  changed?: Bool
  added_lines: List(Text)
  removed_lines: List(Text)
end

enum VcrError
  ChaosRateLimited
  CassetteMiss(url: Text)
  BadConfig(reason: Text)
  UpstreamError(message: Text)
  StorageError(message: Text)
end

actor ChaosController
  state ChaosState = ChaosState(
    mode: Record,
    cassette: "default",
    failure_every: 0,
    total_requests: 0,
    injected_429s: 0
  )
  calls configure_vcr, next_decision, current_stats
  every 1.minute clear_chaos_window

  promise
    after.total_requests >= 0 and after.injected_429s >= 0
    after.failure_every >= 0
  end

  promise on next_decision
    after.total_requests >= before.total_requests
  end
end

fn configure_vcr(state: ChaosState, _now: Time, cfg: ModeConfig) : Result(
  (ChaosState, VcrStats),
  VcrError
)
  return Error(BadConfig(reason: "cassette name cannot be empty")) if cfg.cassette.trim.empty?
  return Error(BadConfig(reason: "failure_every must be >= 0")) if cfg.failure_every < 0
  next = ChaosState(
    mode: cfg.mode,
    cassette: cfg.cassette.trim,
    failure_every: cfg.failure_every,
    total_requests: state.total_requests,
    injected_429s: state.injected_429s
  )
  Ok((next, to_stats(next)))
end

fn next_decision(state: ChaosState, _now: Time) : (ChaosState, ChaosDecision)
  next_total = state.total_requests.saturating_add(1)
  inject = state.failure_every > 0 and next_total % state.failure_every == 0
  next_429s = if inject: state.injected_429s.saturating_add(1) else: state.injected_429s
  next = ChaosState(
    mode: state.mode,
    cassette: state.cassette,
    failure_every: state.failure_every,
    total_requests: next_total,
    injected_429s: next_429s
  )
  (next, ChaosDecision(mode: state.mode, cassette: state.cassette, inject_429?: inject))
end

fn current_stats(state: ChaosState, _now: Time) : VcrStats
  to_stats(state)
end

fn clear_chaos_window(state: ChaosState, _now: Time) : ChaosState
  ChaosState(
    mode: state.mode,
    cassette: state.cassette,
    failure_every: state.failure_every,
    total_requests: state.total_requests,
    injected_429s: 0
  )
end

fn to_stats(s: ChaosState) : VcrStats
  VcrStats(
    mode: s.mode,
    cassette: s.cassette,
    failure_every: s.failure_every,
    total_requests: s.total_requests,
    injected_429s: s.injected_429s
  )
end

api
  port 8101
  sqlite db from "vcr.sqlite"
  actor chaos: ChaosController

  post "/vcr/mode" post_mode
  post "/vcr/proxy" post_proxy
  get "/vcr/diff/:cassette" get_diff
  get "/vcr/stats" get_vcr_stats

  status ChaosRateLimited = 429
  status CassetteMiss(_) = 404
  status BadConfig(_) = 400
  status UpstreamError(_) = 502
  status StorageError(_) = 500
end

fn init_vcr_db(db: Sqlite.Database) : Result(Unit, VcrError)
  sql = [
    "CREATE TABLE IF NOT EXISTS vcr_entries (",
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,",
    "  cassette TEXT NOT NULL,",
    "  method TEXT NOT NULL,",
    "  url TEXT NOT NULL,",
    "  req_body TEXT NOT NULL,",
    "  status INT NOT NULL,",
    "  resp_body TEXT NOT NULL",
    ")"
  ].join("")
  db.run(sql, []).map_error(fn(e) StorageError(message: "#{e}") end)
end

fn cell_str(row: List(Sqlite.Value), idx: Int) : Text
  if row.get(idx) is Some(Sqlite.String(s)): s else: ""
end

fn cell_int(row: List(Sqlite.Value), idx: Int) : Int
  if row.get(idx) is Some(Sqlite.Integer(n)): n else: 0
end

fn normalize_payload(raw: Text) : Text
  case Json.parse(raw)
    Ok(doc): doc.pretty
    Error(_): raw
  end
end

## Configures cassette name, record/replay mode, and deterministic 429 chaos rate.
fn post_mode(chaos: ChaosController, cfg: ModeConfig) : Result(VcrStats, VcrError)
  chaos.configure_vcr(cfg)
end

## Proxies an HTTP request in Record mode or replays from SQLite in Replay mode.
fn post_proxy(chaos: ChaosController, db: Sqlite.Database, net: Network, req: ProxyRequest) : Result(
  ProxyResult,
  VcrError
)
  try init_vcr_db(db)
  decision = chaos.next_decision
  return Error(ChaosRateLimited) if decision.inject_429?

  method = req.method.trim.ascii_uppercase
  req_body = req.body or ""

  case decision.mode
    Replay:
      rows = try db.query(
        [
          "SELECT status, resp_body FROM vcr_entries ",
          "WHERE cassette = ? AND method = ? AND url = ? AND req_body = ? ",
          "ORDER BY id DESC LIMIT 1"
        ].join(""),
        [
          Sqlite.String(decision.cassette),
          Sqlite.String(method),
          Sqlite.String(req.url),
          Sqlite.String(req_body)
        ]
      ).map_error(fn(e) StorageError(message: "#{e}") end)
      row = try rows.rows.first.or_fail(CassetteMiss(url: req.url))
      Ok(ProxyResult(source: "cassette", upstream_status: cell_int(row, 0), body: cell_str(row, 1)))
    Record:
      base = Http.request(method, req.url)
      built = if req_body.empty?: base else: base.text(req_body)
      resp = try built.send(net).map_error(fn(e) UpstreamError(message: "#{e}") end)
      body_txt = resp.text or ""
      try db.run(
        [
          "INSERT INTO vcr_entries (cassette, method, url, req_body, status, resp_body) ",
          "VALUES (?, ?, ?, ?, ?, ?)"
        ].join(""),
        [
          Sqlite.String(decision.cassette),
          Sqlite.String(method),
          Sqlite.String(req.url),
          Sqlite.String(req_body),
          Sqlite.Integer(resp.status),
          Sqlite.String(body_txt)
        ]
      ).map_error(fn(e) StorageError(message: "#{e}") end)
      Ok(ProxyResult(source: "live", upstream_status: resp.status, body: body_txt))
  end
end

## Inspects payload drift across the two latest recorded snapshots for a URL.
fn get_diff(db: Sqlite.Database, cassette: Text, url: Text) : Result(DriftReport, VcrError)
  try init_vcr_db(db)
  rows = try db.query(
    "SELECT resp_body FROM vcr_entries WHERE cassette = ? AND url = ? ORDER BY id DESC LIMIT 2",
    [Sqlite.String(cassette), Sqlite.String(url)]
  ).map_error(fn(e) StorageError(message: "#{e}") end)
  return Error(CassetteMiss(url: url)) if rows.rows.empty?
  newest = normalize_payload(cell_str(rows.rows.first or [], 0))
  if rows.rows.size < 2
    return Ok(
      DriftReport(
        cassette: cassette,
        url: url,
        snapshots: 1,
        changed?: false,
        added_lines: [],
        removed_lines: []
      )
    )
  end
  older = normalize_payload(cell_str(rows.rows.get(1) or [], 0))
  old_set = older.lines.to_set
  new_set = newest.lines.to_set
  added = newest.lines.filter(fn(l) not old_set.has?(l) end)
  removed = older.lines.filter(fn(l) not new_set.has?(l) end)
  Ok(
    DriftReport(
      cassette: cassette,
      url: url,
      snapshots: 2,
      changed?: newest != older,
      added_lines: added,
      removed_lines: removed
    )
  )
end

## Returns current VCR mode and chaos telemetry.
fn get_vcr_stats(chaos: ChaosController) : VcrStats
  chaos.current_stats
end

test "record, offline cassette replay, deterministic 429 chaos, and payload drift diff"
  sqlite = Sqlite.in_memory()
  chaos = Actor.start(ChaosController)
  target_url = "https://api.stripe.example/v1/charges"
  up_req = Http.get(target_url)

  net_v1 = Network.fake(
    Http.replies([(up_req, Http.reply(200, "{\"id\":\"ch_1\",\"amount\":1000}"))])
  )

  assert ApiTest.send(
    Main,
    Api.post("/vcr/mode").with_json(ModeConfig(mode: Record, cassette: "stripe", failure_every: 0)),
    sqlite,
    chaos
  ).status ==
    200

  rec1 = ApiTest.send(
    Main,
    Api.post("/vcr/proxy").with_json(ProxyRequest(method: "GET", url: target_url, body: None)),
    sqlite,
    net_v1,
    chaos
  )
  assert rec1.status == 200
  assert Json.decode[ProxyResult](rec1.text) ==
    Ok(ProxyResult(source: "live", upstream_status: 200, body: "{\"id\":\"ch_1\",\"amount\":1000}"))

  assert ApiTest.send(
    Main,
    Api.post("/vcr/mode").with_json(ModeConfig(mode: Replay, cassette: "stripe", failure_every: 2)),
    sqlite,
    chaos
  ).status ==
    200

  chaos_hit = ApiTest.send(
    Main,
    Api.post("/vcr/proxy").with_json(ProxyRequest(method: "GET", url: target_url, body: None)),
    sqlite,
    Network.fake({}),
    chaos
  )
  assert chaos_hit.status == 429

  replay_ok = ApiTest.send(
    Main,
    Api.post("/vcr/proxy").with_json(ProxyRequest(method: "GET", url: target_url, body: None)),
    sqlite,
    Network.fake({}),
    chaos
  )
  assert replay_ok.status == 200
  assert Json.decode[ProxyResult](replay_ok.text) ==
    Ok(
      ProxyResult(
        source: "cassette",
        upstream_status: 200,
        body: "{\"id\":\"ch_1\",\"amount\":1000}"
      )
    )

  assert ApiTest.send(
    Main,
    Api.post("/vcr/mode").with_json(ModeConfig(mode: Record, cassette: "stripe", failure_every: 0)),
    sqlite,
    chaos
  ).status ==
    200

  net_v2 = Network.fake(
    Http.replies([
      (up_req, Http.reply(200, "{\"id\":\"ch_1\",\"amount\":1250,\"currency\":\"usd\"}"))
    ])
  )
  assert ApiTest.send(
    Main,
    Api.post("/vcr/proxy").with_json(ProxyRequest(method: "GET", url: target_url, body: None)),
    sqlite,
    net_v2,
    chaos
  ).status ==
    200

  diff_resp = ApiTest.send(
    Main,
    Api.get("/vcr/diff/stripe").with_query("url", target_url),
    sqlite,
    chaos
  )
  assert diff_resp.status == 200
  assert Json.decode[DriftReport](diff_resp.text) is Ok(report)
  assert report.changed? and report.snapshots == 2
  assert report.added_lines.any?(fn(l) l.contains?("currency") end)
end

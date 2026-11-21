## `mo-pulse` — Self-hosted error tracker, batched structured log sink with SQLite
## inverted full-text search, and HTTPS uptime monitor composing three in-process actors
## (`AlertDedup`, `BatchWriter`, and `UptimePinger`).
module Main
use Api
use Batch{BatchWriter, BufferedLog}
use Dedup{AlertDedup}
use Http
use Json
use Pinger{UptimePinger, TargetHealth}
use Unicode16

struct ErrorEvent
  service_name: Text
  fingerprint: Text
  message: Text
end

struct ErrorReceipt
  fingerprint: Text
  alerted?: Bool
  occurrences: Int
end

struct FlushSummary
  flushed: Int
end

struct LogHit
  id: Int
  service_name: Text
  level: Text
  message: Text
end

struct MonitorRegistration
  name: Text
  url: Text
end

struct PulseOverview
  suppressed_alerts: Int
  pending_logs: Int
  targets: List(TargetHealth)
end

enum PulseApiError
  InvalidInput(reason: Text)
  UnknownMonitor(name: Text)
  PulseDbFailed(message: Text)
end

api
  port 8104
  sqlite db from "pulse.sqlite"
  actor dedup: AlertDedup
  actor batch: BatchWriter
  actor pinger: UptimePinger

  post "/errors" post_error
  post "/logs" post_log
  post "/logs/flush" post_flush_logs
  get "/logs/search" get_search_logs
  post "/monitors" post_monitor
  post "/monitors/:name/check" post_check_monitor
  get "/overview" get_overview

  status InvalidInput(_) = 400
  status UnknownMonitor(_) = 404
  status PulseDbFailed(_) = 500
end

## Extracts lowercase alphanumeric/Unicode search tokens for inverted full-text indexing.
fn tokenize_search(text: Text) : List(Text)
  promise
    result.all?(fn(tok) not tok.empty? end)
  end

  examples
    tokenize_search("Sqlite WAL lock timeout!") == ["sqlite", "wal", "lock", "timeout"]
  end

  norm = Unicode16.lowercase(text)
  cleaned = Text.from_chars(
    norm.chars.map(fn(c) if Unicode16.letter?(c) or Unicode16.digit?(c): c else: ' ' end)
  )
  cleaned.words.unique
end

fn init_pulse_db(db: Sqlite.Database) : Result(Unit, PulseApiError)
  s1 = [
    "CREATE TABLE IF NOT EXISTS incidents (",
    "  fingerprint TEXT PRIMARY KEY,",
    "  service_name TEXT NOT NULL,",
    "  message TEXT NOT NULL,",
    "  occurrences INT NOT NULL",
    ")"
  ].join("")
  s2 = [
    "CREATE TABLE IF NOT EXISTS logs (",
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,",
    "  service_name TEXT NOT NULL,",
    "  level TEXT NOT NULL,",
    "  message TEXT NOT NULL",
    ")"
  ].join("")
  s3 = [
    "CREATE TABLE IF NOT EXISTS log_tokens (",
    "  token TEXT NOT NULL,",
    "  log_id INT NOT NULL,",
    "  PRIMARY KEY (token, log_id)",
    ")"
  ].join("")
  try db.run(s1, []).map_error(fn(e) PulseDbFailed(message: "#{e}") end)
  try db.run(s2, []).map_error(fn(e) PulseDbFailed(message: "#{e}") end)
  db.run(s3, []).map_error(fn(e) PulseDbFailed(message: "#{e}") end)
end

fn cell_str(row: List(Sqlite.Value), idx: Int) : Text
  if row.get(idx) is Some(Sqlite.String(s)): s else: ""
end

fn cell_int(row: List(Sqlite.Value), idx: Int) : Int
  if row.get(idx) is Some(Sqlite.Integer(n)): n else: 0
end

## Ingests an exception/error event with AlertDedup actor cooldown deduplication.
fn post_error(dedup: AlertDedup, db: Sqlite.Database, ev: ErrorEvent) : Result(
  ErrorReceipt,
  PulseApiError
)
  return Error(InvalidInput(reason: "fingerprint required")) if ev.fingerprint.trim.empty?
  try init_pulse_db(db)
  outcome = dedup.observe_fingerprint(ev.fingerprint.trim)
  try db.run(
    [
      "INSERT INTO incidents (fingerprint, service_name, message, occurrences) ",
      "VALUES (?, ?, ?, ?) ",
      "ON CONFLICT(fingerprint) DO UPDATE SET occurrences = excluded.occurrences, message = excluded.message"
    ].join(""),
    [
      Sqlite.String(ev.fingerprint.trim),
      Sqlite.String(ev.service_name),
      Sqlite.String(ev.message),
      Sqlite.Integer(outcome.occurrences)
    ]
  ).map_error(fn(e) PulseDbFailed(message: "#{e}") end)
  Ok(
    ErrorReceipt(
      fingerprint: ev.fingerprint.trim,
      alerted?: outcome.alert_now?,
      occurrences: outcome.occurrences
    )
  )
end

## Buffers a structured log entry in BatchWriter actor memory.
fn post_log(batch: BatchWriter, entry: BufferedLog) : Result(Int, PulseApiError)
  return Error(InvalidInput(reason: "message required")) if entry.message.trim.empty?
  Ok(batch.enqueue_log(entry))
end

## Flushes buffered logs from BatchWriter into SQLite and indexes search tokens.
fn post_flush_logs(batch: BatchWriter, db: Sqlite.Database) : Result(FlushSummary, PulseApiError)
  try init_pulse_db(db)
  drained = batch.drain_logs
  for entry in drained
    try db.run(
      "INSERT INTO logs (service_name, level, message) VALUES (?, ?, ?)",
      [Sqlite.String(entry.service_name), Sqlite.String(entry.level), Sqlite.String(entry.message)]
    ).map_error(fn(e) PulseDbFailed(message: "#{e}") end)
    id_rows = try db.query("SELECT last_insert_rowid()", []).map_error(
      fn(e) PulseDbFailed(message: "#{e}") end
    )
    log_id = cell_int(id_rows.rows.first or [], 0)
    for tok in tokenize_search("#{entry.service_name} #{entry.level} #{entry.message}")
      try db.run(
        "INSERT INTO log_tokens (token, log_id) VALUES (?, ?) ON CONFLICT(token, log_id) DO NOTHING",
        [Sqlite.String(tok), Sqlite.Integer(log_id)]
      ).map_error(fn(e) PulseDbFailed(message: "#{e}") end)
    end
  end
  Ok(FlushSummary(flushed: drained.size))
end

## Searches indexed logs across all query terms using the inverted token index.
fn get_search_logs(db: Sqlite.Database, q: Text) : Result(List(LogHit), PulseApiError)
  try init_pulse_db(db)
  terms = tokenize_search(q)
  return Ok([]) if terms.empty?
  first_term = terms.first or ""
  rows = try db.query(
    [
      "SELECT l.id, l.service_name, l.level, l.message ",
      "FROM logs l JOIN log_tokens t ON l.id = t.log_id ",
      "WHERE t.token = ? ORDER BY l.id DESC"
    ].join(""),
    [Sqlite.String(first_term)]
  ).map_error(fn(e) PulseDbFailed(message: "#{e}") end)
  extra_terms = terms.drop(1)
  hits = rows.rows.map(fn(r)
    LogHit(
      id: cell_int(r, 0),
      service_name: cell_str(r, 1),
      level: cell_str(r, 2),
      message: cell_str(r, 3)
    )
  end).filter(fn(h)
    doc_toks = tokenize_search("#{h.service_name} #{h.level} #{h.message}")
    extra_terms.all?(fn(t) doc_toks.contains?(t) end)
  end)
  Ok(hits)
end

## Registers an HTTPS endpoint with UptimePinger.
fn post_monitor(pinger: UptimePinger, reg: MonitorRegistration) : Result(
  TargetHealth,
  PulseApiError
)
  return Error(InvalidInput(reason: "name and url required")) if reg.name.trim.empty? or
    reg.url.trim.empty?
  Ok(pinger.register_target(reg.name.trim, reg.url.trim))
end

## Executes an active HTTPS health probe and updates UptimePinger state.
fn post_check_monitor(pinger: UptimePinger, net: Network, name: Text) : Result(
  TargetHealth,
  PulseApiError
)
  target = try pinger.all_targets.find(fn(t) t.name == name end).or_fail(UnknownMonitor(name: name))
  healthy = case Http.get(target.url).send(net)
    Ok(resp): resp.ok?
    Error(_): false
  end
  case pinger.record_probe(name, healthy)
    Some(updated): Ok(updated)
    None: Error(UnknownMonitor(name: name))
  end
end

## Aggregated multi-actor operational overview.
fn get_overview(dedup: AlertDedup, batch: BatchWriter, pinger: UptimePinger) : PulseOverview
  PulseOverview(
    suppressed_alerts: dedup.suppressed_total,
    pending_logs: batch.pending_count,
    targets: pinger.all_targets
  )
end

test "three-actor composition: alert dedup, batched log search, and HTTPS uptime probing"
  sqlite = Sqlite.in_memory()
  dedup = Actor.start(AlertDedup)
  batch = Actor.start(BatchWriter)
  pinger = Actor.start(UptimePinger)

  e1 = ApiTest.send(
    Main,
    Api.post("/errors").with_json(
      ErrorEvent(service_name: "billing", fingerprint: "E_STRIPE_TIMEOUT", message: "timeout 30s")
    ),
    sqlite,
    dedup,
    batch,
    pinger
  )
  assert Json.decode[ErrorReceipt](e1.text) ==
    Ok(ErrorReceipt(fingerprint: "E_STRIPE_TIMEOUT", alerted?: true, occurrences: 1))

  e2 = ApiTest.send(
    Main,
    Api.post("/errors").with_json(
      ErrorEvent(service_name: "billing", fingerprint: "E_STRIPE_TIMEOUT", message: "timeout 30s")
    ),
    sqlite,
    dedup,
    batch,
    pinger
  )
  assert Json.decode[ErrorReceipt](e2.text) ==
    Ok(ErrorReceipt(fingerprint: "E_STRIPE_TIMEOUT", alerted?: false, occurrences: 2))

  assert ApiTest.send(
    Main,
    Api.post("/logs").with_json(
      BufferedLog(
        service_name: "auth",
        level: "error",
        message: "JWT signature expired for user 42"
      )
    ),
    sqlite,
    dedup,
    batch,
    pinger
  ).status ==
    200
  assert ApiTest.send(
    Main,
    Api.post("/logs").with_json(
      BufferedLog(service_name: "worker", level: "info", message: "Snapshot checkpoint finished")
    ),
    sqlite,
    dedup,
    batch,
    pinger
  ).status ==
    200

  flush_resp = ApiTest.send(Main, Api.post("/logs/flush"), sqlite, dedup, batch, pinger)
  assert Json.decode[FlushSummary](flush_resp.text) == Ok(FlushSummary(flushed: 2))

  search_resp = ApiTest.send(
    Main,
    Api.get("/logs/search").with_query("q", "expired JWT"),
    sqlite,
    dedup,
    batch,
    pinger
  )
  assert Json.decode[List(LogHit)](search_resp.text) ==
    Ok([
      LogHit(
        id: 1,
        service_name: "auth",
        level: "error",
        message: "JWT signature expired for user 42"
      )
    ])

  assert ApiTest.send(
    Main,
    Api.post("/monitors").with_json(
      MonitorRegistration(name: "prod-api", url: "https://api.mo.dev/health")
    ),
    sqlite,
    dedup,
    batch,
    pinger
  ).status ==
    200

  probe_req = Http.get("https://api.mo.dev/health")
  net_down = Network.fake(Http.replies([(probe_req, Http.reply(503, "unavailable"))]))
  chk_resp = ApiTest.send(
    Main,
    Api.post("/monitors/prod-api/check"),
    sqlite,
    net_down,
    dedup,
    batch,
    pinger
  )
  assert Json.decode[TargetHealth](chk_resp.text) is Ok(th)
  assert not th.up? and th.consecutive_failures == 1

  ov_resp = ApiTest.send(Main, Api.get("/overview"), sqlite, dedup, batch, pinger)
  assert Json.decode[PulseOverview](ov_resp.text) is Ok(ov)
  assert ov.suppressed_alerts == 1 and ov.pending_logs == 0 and ov.targets.size == 1
end

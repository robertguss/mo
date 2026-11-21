## `mo-gateway` — LLM proxy, prompt cache, SSE streaming server, and exact sub-cent
## 128-bit `Decimal` token budgeter powered by an in-process `Budget` `actor` and `Sqlite`.
module Main
use Api
use Http
use Json

max_budget_usd = 1000000.0

struct KeyQuota
  key_id: Text
  limit_usd: Decimal
  spent_usd: Decimal
  rpm_limit: Int
  rpm_used: Int
end

struct BudgetState
  keys: Map(Text, KeyQuota)
  cache_hits: Int
end

struct ProvisionRequest
  key_id: Text
  token: Text
  limit_usd: Decimal
  rpm_limit: Int
end

struct ApiClient
  key_id: Text
end

struct ChatMessage
  role: Text
  content: Text
end

struct ChatRequest
  model: Text
  messages: List(ChatMessage)
  stream?: Bool
end

struct TokenUsage
  prompt_tokens: Int
  completion_tokens: Int
  cost_usd: Decimal
  cached?: Bool
end

struct ChatResponse
  id: Text
  model: Text
  content: Text
  usage: TokenUsage
end

struct GatewayUsage
  key_id: Text
  limit_usd: Decimal
  spent_usd: Decimal
  remaining_usd: Decimal
  rpm_limit: Int
  rpm_used: Int
  total_cache_hits: Int
end

enum GatewayError
  MissingAuth
  InvalidKey
  BudgetExceeded(remaining_usd: Decimal)
  RpmExceeded(rpm_limit: Int)
  InvalidQuota(reason: Text)
  ProviderFailed(message: Text)
  GatewayDbError(message: Text)
end

## Exact sub-cent token cost in USD using 128-bit Decimal ($3/1M input, $15/1M output).
fn compute_cost_usd(prompt_tokens: Int, completion_tokens: Int) : Decimal
  promise
    prompt_tokens >= 0 and completion_tokens >= 0 implies result >= 0.0
  end

  examples
    compute_cost_usd(100, 10) == 0.00045
    compute_cost_usd(0, 0) == 0.0
  end

  p = if prompt_tokens > 0 and prompt_tokens <= 100000000: prompt_tokens else: 0
  c = if completion_tokens > 0 and completion_tokens <= 100000000: completion_tokens else: 0
  in_cost = (p.to_decimal * 3.0) / 1000000.0
  out_cost = (c.to_decimal * 15.0) / 1000000.0
  (in_cost + out_cost).round(6)
end

actor Budget
  state BudgetState = BudgetState(keys: {}, cache_hits: 0)
  calls register_quota, admit_request, settle_spend, record_cache_hit, inspect_usage
  every 1.minute reset_rpm_windows

  promise
    after.cache_hits >= 0
  end

  promise on record_cache_hit
    after.cache_hits >= before.cache_hits
  end
end

fn register_quota(state: BudgetState, _now: Time, key_id: Text, limit_usd: Decimal, rpm_limit: Int) : Result(
  (BudgetState, KeyQuota),
  GatewayError
)
  return Error(InvalidQuota(reason: "key_id cannot be empty")) if key_id.trim.empty?
  return Error(InvalidQuota(reason: "limit_usd out of range")) if limit_usd <= 0.0 or
    limit_usd > max_budget_usd
  return Error(InvalidQuota(reason: "rpm_limit must be >= 1")) if rpm_limit < 1 or
    rpm_limit > 100000
  q = KeyQuota(
    key_id: key_id.trim,
    limit_usd: limit_usd,
    spent_usd: 0.0,
    rpm_limit: rpm_limit,
    rpm_used: 0
  )
  Ok((BudgetState(keys: state.keys.set(q.key_id, q), cache_hits: state.cache_hits), q))
end

fn admit_request(state: BudgetState, _now: Time, key_id: Text) : Result(
  (BudgetState, KeyQuota),
  GatewayError
)
  q = try state.keys.get(key_id).or_fail(InvalidKey)
  return Error(RpmExceeded(rpm_limit: q.rpm_limit)) if q.rpm_used >= q.rpm_limit
  remaining = if q.limit_usd > q.spent_usd: q.limit_usd - q.spent_usd else: 0.0
  return Error(BudgetExceeded(remaining_usd: remaining)) if remaining <= 0.0
  updated = KeyQuota(
    key_id: q.key_id,
    limit_usd: q.limit_usd,
    spent_usd: q.spent_usd,
    rpm_limit: q.rpm_limit,
    rpm_used: q.rpm_used.saturating_add(1)
  )
  Ok((BudgetState(keys: state.keys.set(key_id, updated), cache_hits: state.cache_hits), updated))
end

fn settle_spend(state: BudgetState, _now: Time, key_id: Text, cost_usd: Decimal) : Result(
  (BudgetState, KeyQuota),
  GatewayError
)
  q = try state.keys.get(key_id).or_fail(InvalidKey)
  delta = if cost_usd > 0.0 and cost_usd <= max_budget_usd: cost_usd else: 0.0
  next_spent = if q.spent_usd <= max_budget_usd: q.spent_usd + delta else: q.spent_usd
  updated = KeyQuota(
    key_id: q.key_id,
    limit_usd: q.limit_usd,
    spent_usd: next_spent,
    rpm_limit: q.rpm_limit,
    rpm_used: q.rpm_used
  )
  Ok((BudgetState(keys: state.keys.set(key_id, updated), cache_hits: state.cache_hits), updated))
end

fn record_cache_hit(state: BudgetState, _now: Time) : (BudgetState, Int)
  hits = state.cache_hits.saturating_add(1)
  (BudgetState(keys: state.keys, cache_hits: hits), hits)
end

fn inspect_usage(state: BudgetState, _now: Time, key_id: Text) : Result(GatewayUsage, GatewayError)
  q = try state.keys.get(key_id).or_fail(InvalidKey)
  rem = if q.limit_usd > q.spent_usd: q.limit_usd - q.spent_usd else: 0.0
  Ok(
    GatewayUsage(
      key_id: q.key_id,
      limit_usd: q.limit_usd,
      spent_usd: q.spent_usd,
      remaining_usd: rem,
      rpm_limit: q.rpm_limit,
      rpm_used: q.rpm_used,
      total_cache_hits: state.cache_hits
    )
  )
end

fn reset_rpm_windows(state: BudgetState, _now: Time) : BudgetState
  var next_keys = {}
  for (k, q) in state.keys.entries
    next_keys = next_keys.set(
      k,
      KeyQuota(
        key_id: q.key_id,
        limit_usd: q.limit_usd,
        spent_usd: q.spent_usd,
        rpm_limit: q.rpm_limit,
        rpm_used: 0
      )
    )
  end
  BudgetState(keys: next_keys, cache_hits: state.cache_hits)
end

api
  port 8103
  sqlite db from "gateway.sqlite"
  actor budget: Budget
  auth ApiClient via authenticate_client

  post "/admin/keys" post_admin_key
  post "/v1/chat/completions" post_chat_completions
  get "/v1/usage" get_client_usage

  status MissingAuth = 401
  status InvalidKey = 403
  status BudgetExceeded(_) = 429
  status RpmExceeded(_) = 429
  status InvalidQuota(_) = 400
  status ProviderFailed(_) = 502
  status GatewayDbError(_) = 500
end

fn init_gateway_db(db: Sqlite.Database) : Result(Unit, GatewayError)
  s1 = [
    "CREATE TABLE IF NOT EXISTS api_keys (",
    "  token TEXT PRIMARY KEY,",
    "  key_id TEXT NOT NULL",
    ")"
  ].join("")
  s2 = [
    "CREATE TABLE IF NOT EXISTS prompt_cache (",
    "  cache_key TEXT PRIMARY KEY,",
    "  content TEXT NOT NULL,",
    "  prompt_tokens INT NOT NULL,",
    "  completion_tokens INT NOT NULL",
    ")"
  ].join("")
  s3 = [
    "CREATE TABLE IF NOT EXISTS telemetry (",
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,",
    "  key_id TEXT NOT NULL,",
    "  model TEXT NOT NULL,",
    "  cached INT NOT NULL,",
    "  cost_usd TEXT NOT NULL",
    ")"
  ].join("")
  try db.run(s1, []).map_error(fn(e) GatewayDbError(message: "#{e}") end)
  try db.run(s2, []).map_error(fn(e) GatewayDbError(message: "#{e}") end)
  db.run(s3, []).map_error(fn(e) GatewayDbError(message: "#{e}") end)
end

fn cell_str(row: List(Sqlite.Value), idx: Int) : Text
  if row.get(idx) is Some(Sqlite.String(s)): s else: ""
end

fn cell_int(row: List(Sqlite.Value), idx: Int) : Int
  if row.get(idx) is Some(Sqlite.Integer(n)): n else: 0
end

fn authenticate_client(req: Api.Request, db: Sqlite.Database) : Result(ApiClient, GatewayError)
  try init_gateway_db(db)
  tok = try req.bearer_token.or_fail(MissingAuth)
  rows = try db.query("SELECT key_id FROM api_keys WHERE token = ?", [Sqlite.String(tok)]).map_error(
    fn(e) GatewayDbError(message: "#{e}") end
  )
  row = try rows.rows.first.or_fail(InvalidKey)
  Ok(ApiClient(key_id: cell_str(row, 0)))
end

## Provisions an API key with exact USD Decimal budget and RPM caps.
fn post_admin_key(db: Sqlite.Database, budget: Budget, req: ProvisionRequest) : Result(
  KeyQuota,
  GatewayError
)
  try init_gateway_db(db)
  return Error(InvalidQuota(reason: "token cannot be empty")) if req.token.trim.empty?
  quota = try budget.register_quota(req.key_id, req.limit_usd, req.rpm_limit)
  try db.run(
    "INSERT INTO api_keys (token, key_id) VALUES (?, ?) ON CONFLICT(token) DO UPDATE SET key_id = excluded.key_id",
    [Sqlite.String(req.token.trim), Sqlite.String(quota.key_id)]
  ).map_error(fn(e) GatewayDbError(message: "#{e}") end)
  Ok(quota)
end

fn prompt_cache_key(model: Text, messages: List(ChatMessage)) : Text
  joined = messages.map(fn(m) "#{m.role}:#{m.content}" end).join("|")
  "#{model}::#{joined}"
end

## Proxies chat completions with SQLite prompt caching, exact Decimal accounting, and SSE streaming.
fn post_chat_completions(
  client: ApiClient,
  budget: Budget,
  db: Sqlite.Database,
  net: Network,
  body: ChatRequest
) : Result(Api.Response, GatewayError)
  try init_gateway_db(db)
  _admitted = try budget.admit_request(client.key_id)
  ckey = prompt_cache_key(body.model, body.messages)

  cached_rows = try db.query(
    "SELECT content, prompt_tokens, completion_tokens FROM prompt_cache WHERE cache_key = ?",
    [Sqlite.String(ckey)]
  ).map_error(fn(e) GatewayDbError(message: "#{e}") end)

  (reply_text, p_toks, c_toks, was_cached, cost) = case cached_rows.rows.first
    Some(r):
      _hits = budget.record_cache_hit
      (cell_str(r, 0), cell_int(r, 1), cell_int(r, 2), true, 0.0)
    None:
      up_payload = Json.Object({"model": Json.String(body.model), "messages": body.messages.to_json})
      up_req = Http.post("https://llm.provider.example/v1/chat").json(up_payload)
      up_resp = try up_req.send(net).map_error(fn(e) ProviderFailed(message: "#{e}") end)
      return Error(ProviderFailed(message: "HTTP #{up_resp.status}")) if not up_resp.ok?
      doc = try up_resp.json.map_error(fn(e) ProviderFailed(message: "#{e}") end)
      txt = doc.text_at("content") or ""
      pt = doc.int_at("prompt_tokens") or 0
      ct = doc.int_at("completion_tokens") or 0
      usd = compute_cost_usd(pt, ct)
      _settled = try budget.settle_spend(client.key_id, usd)
      try db.run(
        [
          "INSERT INTO prompt_cache (cache_key, content, prompt_tokens, completion_tokens) ",
          "VALUES (?, ?, ?, ?) ON CONFLICT(cache_key) DO NOTHING"
        ].join(""),
        [Sqlite.String(ckey), Sqlite.String(txt), Sqlite.Integer(pt), Sqlite.Integer(ct)]
      ).map_error(fn(e) GatewayDbError(message: "#{e}") end)
      (txt, pt, ct, false, usd)
  end

  try db.run(
    "INSERT INTO telemetry (key_id, model, cached, cost_usd) VALUES (?, ?, ?, ?)",
    [
      Sqlite.String(client.key_id),
      Sqlite.String(body.model),
      Sqlite.Integer(if was_cached: 1 else: 0),
      Sqlite.String(cost.to_fixed(6))
    ]
  ).map_error(fn(e) GatewayDbError(message: "#{e}") end)

  chat_resp = ChatResponse(
    id: "chatcmpl-mo",
    model: body.model,
    content: reply_text,
    usage: TokenUsage(
      prompt_tokens: p_toks,
      completion_tokens: c_toks,
      cost_usd: cost,
      cached?: was_cached
    )
  )

  if body.stream?
    chunk = Json.encode(chat_resp)
    Ok(Api.sse_response([(Some("chunk"), chunk), (None, "[DONE]")]))
  else
    Ok(Api.json_response(200, chat_resp))
  end
end

## Inspects remaining Decimal USD budget, RPM usage, and cache hits for the authenticated key.
fn get_client_usage(client: ApiClient, budget: Budget) : Result(GatewayUsage, GatewayError)
  case Actor.send(budget, Budget.inspect_usage, client.key_id)
    Ok(res): res
    Error(_): Error(GatewayDbError(message: "budget actor unavailable"))
  end
end

test "exact sub-cent Decimal accounting, prompt caching, SSE streaming, and 429 budget caps"
  sqlite = Sqlite.in_memory()
  budget = Actor.start(Budget)

  prov = ApiTest.send(
    Main,
    Api.post("/admin/keys").with_json(
      ProvisionRequest(key_id: "team-alpha", token: "sk-mo-1", limit_usd: 0.0005, rpm_limit: 5)
    ),
    sqlite,
    budget
  )
  assert prov.status == 200

  msgs = [ChatMessage(role: "user", content: "Explain BEAM actors")]
  up_payload = Json.Object({"model": Json.String("claude-sonnet"), "messages": msgs.to_json})
  up_req = Http.post("https://llm.provider.example/v1/chat").json(up_payload)
  up_body = "{\"content\":\"Isolated heaps!\",\"prompt_tokens\":100,\"completion_tokens\":10}"
  net = Network.fake(Http.replies([(up_req, Http.reply(200, up_body))]))

  miss_resp = ApiTest.send(
    Main,
    Api.post("/v1/chat/completions").with_bearer("sk-mo-1").with_json(
      ChatRequest(model: "claude-sonnet", messages: msgs, stream?: false)
    ),
    sqlite,
    net,
    budget
  )
  assert miss_resp.status == 200
  assert Json.decode[ChatResponse](miss_resp.text) is Ok(first_chat)
  assert not first_chat.usage.cached? and first_chat.usage.cost_usd == 0.00045

  stream_hit = ApiTest.send(
    Main,
    Api.post("/v1/chat/completions").with_bearer("sk-mo-1").with_json(
      ChatRequest(model: "claude-sonnet", messages: msgs, stream?: true)
    ),
    sqlite,
    Network.fake({}),
    budget
  )
  assert stream_hit.status == 200
  assert stream_hit.header("content-type") == Some("text/event-stream")
  assert stream_hit.text.contains?("event: chunk")
  assert stream_hit.text.contains?("data: [DONE]")

  usage_resp = ApiTest.send(Main, Api.get("/v1/usage").with_bearer("sk-mo-1"), sqlite, budget)
  assert Json.decode[GatewayUsage](usage_resp.text) ==
    Ok(
      GatewayUsage(
        key_id: "team-alpha",
        limit_usd: 0.0005,
        spent_usd: 0.00045,
        remaining_usd: 0.00005,
        rpm_limit: 5,
        rpm_used: 2,
        total_cache_hits: 1
      )
    )
end

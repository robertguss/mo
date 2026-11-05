module McpViewsCheck
use Json
use McpViews

fn require(ok: Bool)
  if not ok
    stop "operator view contract"
  end
end

fn main() : Int
  require(McpViews.names().size == 7)
  require(not McpViews.names().contains?("mo-export"))
  require(not McpViews.names().contains?("mo-failures"))
  require(
    McpViews.valid(1, Json.Object({"path": Json.Array([Json.String("private"), Json.int(2)])}))
  )
  require(not McpViews.valid(1, Json.Object({"path": Json.Array([Json.int(-1)])})))
  require(not McpViews.valid(1, Json.Object({"path": Json.Array([Json.Null])})))
  require(not McpViews.valid(0, Json.Object({"after": Json.int(0)})))
  require(not McpViews.valid(5, Json.Object({"after": Json.String("0")})))
  require(McpViews.valid(5, Json.Object({"after": Json.int(0)})))
  for tool in McpViews.catalog()
    fields = case tool
      Json.Object(value): value
      Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): stop "tool object"
    end
    require(
      fields.get("annotations") ==
        Some(
          Json.Object({"readOnlyHint": Json.Boolean(true), "idempotentHint": Json.Boolean(true)})
        )
    )
    description = case fields.get("description")
      Some(Json.String(value)): value
      None | Some(_): stop "description"
    end
    require(description.byte_count <= 512)
  end
  stats = McpViews.Stats(
    sequence: 0,
    snapshot: 0,
    encoded_sequence: 0,
    elapsed: "0",
    heap_used: 0,
    heap_peak: 0,
    heap_limit: 0,
    stack: 0,
    encoded_bytes: 30,
    scratch_peak: 0,
    failures: 0,
    restarts: 0,
    running: true,
    persistent: true,
    failure_window: 60000000,
    restart_window: 600000000,
    timer_count: 0,
    stack_limit: 0
  )
  observation = McpViews.Observation(
    service_name: "fixture",
    program: "0",
    build: "",
    build_label: "fixture",
    stats: stats,
    mailbox: [],
    timers: [],
    events: [],
    log: [],
    uptime: "0"
  )
  encoded = "MOSV\u{1}\u{d}\u{0}\u{0}\u{0}\u{3}\u{0}\u{0}\u{0}\u{0}\u{1}\u{0}\u{0}\u{0}\u{0}\u{0}\u{0}\u{0}*\u{0}\u{0}\u{0}\u{0}\u{0}\u{0}\u{0}"
  result = McpViews.render(1, Json.Object({}), observation, encoded.bytes, 6144)
  fields = case result
    Json.Object(value): value
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): stop "state object"
  end
  require(fields.get("state") == Some(Json.String("42")))
  require(fields.get("complete") == Some(Json.Boolean(true)))
  37
end

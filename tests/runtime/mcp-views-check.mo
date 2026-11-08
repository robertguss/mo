module McpViewsCheck
use Json
use McpViews

fn require(ok: Bool)
  if not ok
    stop "operator view contract"
  end
end

fn number(text: Text) : Json.Json
  case Json.parse(text)
    Ok(value): value
    Error(_): stop "number fixture"
  end
end

fn object(value: Json.Json) : Map(Text, Json.Json)
  case value
    Json.Object(fields): fields
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): stop "object"
  end
end

fn items(value: Json.Json) : List(Json.Json)
  case object(value).get("items") or stop "items"
    Json.Array(values): values
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Object(_): stop "items array"
  end
end

fn byte(n: Int) : Text
  require(n >= 0 and n < 128)
  (n.to_char or stop "byte").to_text
end

## Little-endian, every byte below 128 so a Text literal holds it.
fn le(value: Int, width: Int) : Text
  var out = ""
  var rest = value
  for _ in 0..width
    out = "#{out}#{byte(rest % 256)}"
    rest = rest / 256
  end
  out
end

## count copies of element under a List schema; every non-choice schema has one group.
fn saved_list(child: Text, count: Int, element: Text) : Bytes
  schema = "#{byte(14)}#{le(1, 4)}#{child}#{le(1, 4)}#{le(0, 4)}"
  "MOSV#{byte(1)}#{le(schema.byte_count, 4)}#{schema}#{le(count, 4)}#{element.repeat(count)}".bytes
end

fn pages(index: Int, observation: McpViews.Observation, first: Json.Json) : List(Json.Json)
  var arguments = first
  var seen = []
  for _ in 0..100
    page = McpViews.render(index, arguments, observation, "".bytes, 6144)
    require(page.to_text.byte_count <= 6144 + 512)
    shown = items(page)
    require(not shown.empty?)
    seen = seen.concat(shown)
    case object(page).get("next")
      Some(next): arguments = next
      None:
        require(object(page).get("complete") == Some(Json.Boolean(true)))
        return seen
    end
  end
  stop "pages never ended"
end

fn numbers(values: List(Json.Json), key: Text) : List(Text)
  values.map(
    fn(value) case object(value).get(key)
      Some(Json.String(text)): text
      None | Some(_): stop "number field"
    end end
  )
end

fn main() : Int
  failure = [
    "1",
    "broken:0:stop",
    "broken",
    "-1",
    "10",
    "200",
    "1",
    "defect",
    "stop",
    "",
    "0",
    "4",
    "0",
    "9",
    "14"
  ]
  omitted = ["-1", "3", "", "", "", "", "", "", "", "", "", "", "", "", ""]
  failures = object(McpViews.failure_reply(Json.Object({}), failure.concat(omitted)))
  require(failures.get("truncated") == Some(Json.int(3)))
  require(failures.get("next") == Some(Json.Object({"after": Json.int(1)})))
  work = [
    "2",
    "spin:1:work",
    "spin",
    "0",
    "10",
    "200",
    "1",
    "work",
    "work limit",
    "MO_LIMIT_SERVICE_WORK",
    "1048576",
    "0",
    "0",
    "0",
    "0"
  ]
  work_page = object(McpViews.failure_reply(Json.Object({}), work))
  case work_page.get("records")
    Some(Json.Array(rows)):
      row = object(rows.first or stop "work record")
      signature = object(row.get("signature") or stop "signature")
      require(signature.get("class") == Some(Json.String("work")))
      require(signature.get("limit") == Some(Json.String("MO_LIMIT_SERVICE_WORK")))
      require(signature.get("value") == Some(Json.String("1048576")))
    Some(_) | None: stop "records"
  end
  complete_failures = object(McpViews.failure_reply(Json.Object({}), failure))
  require(complete_failures.get("truncated") == Some(Json.int(0)))
  require(complete_failures.get("next") == Some(Json.Null))
  page = object(
    McpViews.export_reply(
      "",
      ["0123456789abcdef0123456789abcdef", "0", "0", "0", "2", "2", "0", "", "failure", "7"],
      "".bytes
    )
  )
  next = page.get("next") or stop "next page"
  require(McpViews.valid(7, next))
  require(McpViews.failure_id(next) == 7)
  require(McpViews.names().size == 12)
  require(McpViews.upgrade_kind(-1) == 0)
  require(McpViews.upgrade_kind(12) == 0)
  for i in 0..9
    require(McpViews.upgrade_kind(i) == 0)
  end
  require(McpViews.upgrade_kind(9) == 1)
  require(McpViews.upgrade_kind(10) == 2)
  require(McpViews.upgrade_kind(11) == 3)
  require(McpViews.names().get(9) == Some("mo-upgrade-open"))
  require(McpViews.names().get(11) == Some("mo-upgrade-commit"))
  up_base = "0".repeat(64)
  up_candidate = "a".repeat(64)
  up_slot = "b".repeat(32)
  require(
    McpViews.valid(
      9,
      Json.Object({
        "base": Json.String(up_base),
        "candidate": Json.String(up_candidate),
        "total": Json.int(10),
        "migration": Json.String("Main.migrate")
      })
    )
  )
  for arguments in [
    Json.Object({"base": Json.String(up_base), "candidate": Json.String(up_candidate)}),
    Json.Object({
      "base": Json.String("0".repeat(63)),
      "candidate": Json.String(up_candidate),
      "total": Json.int(1)
    }),
    Json.Object({
      "base": Json.String("A".repeat(64)),
      "candidate": Json.String(up_candidate),
      "total": Json.int(1)
    }),
    Json.Object({
      "base": Json.String(up_base),
      "candidate": Json.String(up_candidate),
      "total": Json.int(1),
      "migration": Json.String("")
    }),
    Json.Object({
      "base": Json.String(up_base),
      "candidate": Json.String(up_candidate),
      "total": Json.int(1),
      "slot": Json.String(up_slot)
    })
  ]
    require(not McpViews.valid(9, arguments))
  end
  up_page = Json.Object({
    "slot": Json.String(up_slot),
    "index": Json.int(0),
    "offset": Json.int(0),
    "total": Json.int(3),
    "bytes": Json.String("YWJj")
  })
  require(McpViews.valid(10, up_page))
  require(not McpViews.valid(10, Json.Object({"slot": Json.String(up_slot)})))
  require(
    not McpViews.valid(
      10,
      Json.Object({
        "slot": Json.String(up_slot),
        "index": Json.int(0),
        "offset": Json.int(0),
        "total": Json.int(3),
        "bytes": Json.String("A".repeat(10925))
      })
    )
  )
  require(
    McpViews.valid(11, Json.Object({"slot": Json.String(up_slot), "base": Json.String(up_base)}))
  )
  require(not McpViews.valid(11, Json.Object({"slot": Json.String(up_slot)})))
  require(McpViews.upgrade_arguments(up_page) == ["", "", "3", "", up_slot, "0", "0", "YWJj"])
  require(McpViews.names().contains?("mo-export"))
  require(McpViews.names().contains?("mo-failures"))
  require(
    McpViews.valid(1, Json.Object({"path": Json.Array([Json.String("private"), Json.int(2)])}))
  )
  require(not McpViews.valid(1, Json.Object({"path": Json.Array([Json.int(-1)])})))
  require(not McpViews.valid(1, Json.Object({"path": Json.Array([Json.Null])})))
  require(not McpViews.valid(0, Json.Object({"after": Json.int(0)})))
  require(not McpViews.valid(5, Json.Object({"after": Json.String("0")})))
  require(McpViews.valid(5, Json.Object({"after": Json.int(0)})))
  require(McpViews.valid(5, Json.Object({"after": Json.int(1), "before": Json.int(9)})))
  require(McpViews.valid(2, Json.Object({"offset": Json.int(3)})))
  require(McpViews.valid(3, Json.Object({"offset": Json.int(300)})))
  require(not McpViews.valid(6, Json.Object({"before": Json.int(1)})))
  require(not McpViews.valid(0, Json.Object({"offset": Json.int(1)})))
  for spelling in ["-0", "1.0", "1e2", "-1"]
    require(not McpViews.valid(2, Json.Object({"offset": number(spelling)})))
    require(not McpViews.valid(1, Json.Object({"path": Json.Array([number(spelling)])})))
  end
  require(McpViews.offset(Json.Object({"offset": Json.int(7)})) == 7)
  require(McpViews.offset(Json.Object({})) == 0)
  require(McpViews.valid(7, Json.Object({"kind": Json.String("state")})))
  require(not McpViews.valid(7, Json.Object({})))
  require(not McpViews.valid(7, Json.Object({"kind": Json.String("failure")})))
  for tool in McpViews.catalog()
    fields = case tool
      Json.Object(value): value
      Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): stop "tool object"
    end
    require(
      fields.get("annotations") ==
        Some(
          Json.Object(
            if fields.get("name") == Some(Json.String("mo-export")): {
              "readOnlyHint": Json.Boolean(true),
              "idempotentHint": Json.Boolean(true),
              "openWorldHint": Json.Boolean(false)
            } else if fields.get("name") == Some(Json.String("mo-upgrade-open")) or
              fields.get("name") == Some(Json.String("mo-upgrade-page")) or
              fields.get("name") == Some(Json.String("mo-upgrade-commit")): {
                "readOnlyHint": Json.Boolean(false),
                "destructiveHint": Json.Boolean(
                  fields.get("name") == Some(Json.String("mo-upgrade-commit"))
                ),
                "idempotentHint": Json.Boolean(false),
                "openWorldHint": Json.Boolean(false)
              } else: {"readOnlyHint": Json.Boolean(true), "idempotentHint": Json.Boolean(true)}
          )
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
    stack_limit: 0,
    pinned_export_bytes: 0,
    retained_failures: 0,
    evicted_kinds: "0"
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
  views(observation)
  states(observation)
  overviews(observation)
  37
end

fn overviews(base: McpViews.Observation)
  open = object(McpViews.render(0, Json.Object({}), base, "".bytes, 6144))
  require(open.get("retained_failures") == Some(Json.int(0)))
  require(open.get("evicted_kinds_since_open") == Some(Json.String("0")))
  require(open.get("failure_capture_available") == Some(Json.Boolean(true)))
  var unread = base
  unread.stats.retained_failures = -1
  refused = object(McpViews.render(0, Json.Object({}), unread, "".bytes, 6144))
  require(refused.get("retained_failures") == Some(Json.Null))
  require(refused.get("evicted_kinds_since_open") == Some(Json.Null))
  require(refused.get("failure_capture_available") == Some(Json.Boolean(false)))
  var memory = base
  memory.stats.persistent = false
  transient = object(McpViews.render(0, Json.Object({}), memory, "".bytes, 6144))
  require(transient.get("failure_capture_available") == Some(Json.Boolean(false)))
end

fn views(base: McpViews.Observation)
  long = "h".repeat(120)
  events = (1..61).to_list.map(
    fn(i) McpViews.Event(
      number: i.to_text,
      kind: "failure",
      reason: "service handler failed",
      handler: long,
      arrival: i,
      elapsed: i.to_text,
      record: "0",
      unavailable: ""
    ) end
  )
  var observed = base
  observed.events = events
  newest = numbers(pages(5, observed, Json.Object({})), "number")
  require(newest == (1..61).to_list.reverse.map(fn(i) i.to_text end))
  window = numbers(pages(5, observed, Json.Object({"after": Json.int(10)})), "number")
  require(window == (11..61).to_list.reverse.map(fn(i) i.to_text end))
  messages = (1..101).to_list.map(
    fn(i) McpViews.Message(
      handler: long,
      timer: false,
      arrival: i,
      number: i.to_text,
      argument_bytes: 3,
      size_available: true
    ) end
  )
  var logs = base
  logs.log = messages
  logged = numbers(pages(6, logs, Json.Object({})), "sequence")
  require(logged == (1..101).to_list.map(fn(i) i.to_text end))
  var queued = base
  queued.mailbox = messages.take(64)
  mailbox = numbers(pages(2, queued, Json.Object({})), "sequence")
  require(mailbox == (1..65).to_list.map(fn(i) i.to_text end))
  timers = (0..256).to_list.map(
    fn(i) McpViews.Timer(name: "t#{i}", interval: "1", next: "2", waiting: false) end
  )
  var copied = base
  copied.stats.timer_count = 300
  copied.timers = timers
  page = McpViews.render(3, Json.Object({}), copied, "".bytes, 6144)
  shown = items(page).size
  require(shown > 0 and shown < 256)
  require(object(page).get("truncated") == Some(Json.int(300 - shown)))
  require(object(page).get("next") == Some(Json.Object({"offset": Json.int(shown)})))
  require(items(page).all?(fn(item) object(item).get("truncated") == None end))
end

fn states(observation: McpViews.Observation)
  text = "#{byte(10)}#{le(0, 4)}#{le(1, 4)}#{le(0, 4)}"
  int = "#{byte(3)}#{le(0, 4)}#{le(1, 4)}#{le(0, 4)}"
  count = 32639
  empties = saved_list(text, count, le(0, 4))
  whole = object(McpViews.render(1, Json.Object({}), observation, empties, 6144))
  state = whole.get("state") or stop "state"
  require(state.to_text.byte_count <= 6144)
  shown = items(state).size
  require(shown > 0 and object(state).get("truncated") == Some(Json.int(count - shown)))
  require(whole.get("complete") == Some(Json.Boolean(false)))
  last = Json.Object({"path": Json.Array([Json.int(count - 1)])})
  found = object(McpViews.render(1, last, observation, empties, 6144))
  require(
    found.get("state") == Some(Json.String("")) and
      found.get("complete") == Some(Json.Boolean(true))
  )
  past = Json.Object({"path": Json.Array([Json.int(count)])})
  require(object(McpViews.render(1, past, observation, empties, 6144)).get("unavailable") != None)
  var values = ""
  for i in 0..count
    values = "#{values}#{le(i % 128, 8)}"
  end
  schema = "#{byte(14)}#{le(1, 4)}#{int}#{le(1, 4)}#{le(0, 4)}"
  ints = "MOSV#{byte(1)}#{le(schema.byte_count, 4)}#{schema}#{le(count, 4)}#{values}".bytes
  found_int = object(McpViews.render(1, last, observation, ints, 6144))
  require(found_int.get("state") == Some(Json.String(((count - 1) % 128).to_text)))
  note = "n".repeat(1799)
  long = saved_list(text, 2, "#{le(1799, 4)}#{note}")
  rendered = object(McpViews.render(1, Json.Object({}), observation, long, 6144))
  require(rendered.get("state") == Some(Json.Array([Json.String(note), Json.String(note)])))
  big = saved_list(text, 2, "#{le(7000, 4)}#{"b".repeat(7000)}")
  cut = object(McpViews.render(1, Json.Object({}), observation, big, 6144))
  marker = Json.Object({"truncated_bytes": Json.int(7000)})
  require(cut.get("state") == Some(Json.Array([marker, marker])))
end

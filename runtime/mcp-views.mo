module McpViews
expose names,
offline_value,
catalog,
valid,
offset,
render,
export_id,
failure_id,
failure_after,
failure_reply,
export_reply,
Observation(..),
Stats(..),
Message(..),
Timer(..),
Event(..)
use Json

struct Stats
  sequence: Int
  snapshot: Int
  encoded_sequence: Int
  elapsed: Text
  heap_used: Int
  heap_peak: Int
  heap_limit: Int
  stack: Int
  encoded_bytes: Int
  scratch_peak: Int
  failures: Int
  restarts: Int
  running: Bool
  persistent: Bool
  failure_window: Int
  restart_window: Int
  timer_count: Int
  stack_limit: Int
  pinned_export_bytes: Int
  retained_failures: Int
  evicted_kinds: Text
end

struct Message
  handler: Text
  timer: Bool
  arrival: Int
  number: Text
  argument_bytes: Int
  size_available: Bool
end

struct Timer
  name: Text
  interval: Text
  next: Text
  waiting: Bool
end

struct Event
  number: Text
  kind: Text
  reason: Text
  handler: Text
  arrival: Int
  elapsed: Text
  record: Text
  unavailable: Text
end

struct Observation
  service_name: Text
  program: Text
  build: Text
  build_label: Text
  stats: Stats
  mailbox: List(Message)
  timers: List(Timer)
  events: List(Event)
  log: List(Message)
  uptime: Text
end

fn names() : List(Text)
  [
    "mo-overview",
    "mo-state",
    "mo-mailbox",
    "mo-timers",
    "mo-memory",
    "mo-supervisor",
    "mo-log",
    "mo-export",
    "mo-failures"
  ]
end

## MO_LIMIT_SAVED_DEPTH; test_protocol_server pins the two together.
fn saved_depth() : Int
  128
end

fn arguments_of(index: Int) : List(Text)
  if index == 1: ["path"] else if index == 2 or index == 3: ["offset"] else if index == 5: [
    "after",
    "before"
  ] else if index == 6: ["after"] else if index == 7: ["kind", "failure", "export", "offset"] else if index ==
    8: ["after"] else: []
end

fn catalog() : List(Json.Json)
  descriptions = [
    "Runtime identities, recovery position and live counters. Independent observations are not an atomic snapshot. Retained failure records are available from mo-failures. State export uses mo-export.",
    "Current saved state, including private fields. Optional path contains field names or nonnegative indexes. Large views explicitly truncate; use mo-export for complete saved bytes.",
    "Queued handler names, arrival times and encoded argument sizes where available, never argument values. A truncated page names the next offset. Does not execute handlers.",
    "Timer names, intervals, next due elapsed times and waiting ticks. A truncated page names the next offset. Does not execute handlers.",
    "Live heap use, peak and limit, stack capacity and encoded state bytes. Includes native export pin bytes. Retained diagnostics use separate bounded native storage.",
    "Retained in-memory supervisor events, newest first, optionally after or before an event number. A truncated page names the next arguments. Events are lost on process exit; capture refusals include an explicit reason.",
    "Committed message metadata since the last snapshot, oldest first, optionally after a sequence. Older successful messages are not retained. Does not execute handlers.",
    "Export saved state as immutable MOEX v1 pages. Start with kind state; continue with returned export id and offset. Pages expire after 60 seconds idle or replacement. Final page releases the pin. Use mo fetch on the service host to obtain a verified private state artifact. Failure export selects a retained record id.",
    "Retained failure records, oldest first; after selects a later page. Capture refusals remain in mo-supervisor."
  ]
  tool_names = names()
  integer = Json.Object({"type": Json.String("integer"), "minimum": Json.int(0)})
  var tools = []
  for i in 0..tool_names.size
    var properties = {}
    for key in arguments_of(i)
      properties = properties.set(
        key,
        if key == "path": Json.Object({
          "type": Json.String("array"),
          "maxItems": Json.int(saved_depth()),
          "items": Json.Object({
            "anyOf": Json.Array([Json.Object({"type": Json.String("string")}), integer])
          })
        }) else if key == "kind": Json.Object({
          "type": Json.String("string"),
          "enum": Json.Array([Json.String("state"), Json.String("failure")])
        }) else if key == "export": Json.Object({
          "type": Json.String("string"),
          "pattern": Json.String("^[0-9a-f]{32}$")
        }) else: integer
      )
    end
    tools = tools.append(
      Json.Object({
        "name": Json.String(tool_names.get(i) or stop "view name"),
        "description": Json.String(descriptions.get(i) or stop "view description"),
        "inputSchema": Json.Object({
          "type": Json.String("object"),
          "properties": Json.Object(properties),
          "required": Json.Array(if i == 7: [Json.String("kind")] else: []),
          "additionalProperties": Json.Boolean(false)
        }),
        "annotations": Json.Object(
          if i == 7: {
            "readOnlyHint": Json.Boolean(true),
            "idempotentHint": Json.Boolean(true),
            "openWorldHint": Json.Boolean(false)
          } else: {"readOnlyHint": Json.Boolean(true), "idempotentHint": Json.Boolean(true)}
        )
      })
    )
  end
  tools
end

## A nonnegative integer written the one way Json.int writes it, so "-0", "1.0" and "1e2" fail.
fn whole(value: Json.Json) : Option(Int)
  case value
    Json.Number(_):
      n = try value.to_text.to_int
      if n >= 0 and Json.int(n) == value: Some(n) else: None
    Json.Null | Json.Boolean(_) | Json.String(_) | Json.Array(_) | Json.Object(_): None
  end
end

fn argument(fields: Map(Text, Json.Json), key: Text) : Option(Int)
  whole(try fields.get(key))
end

fn valid(index: Int, arguments: Json.Json) : Bool
  fields = case arguments
    Json.Object(value): value
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return false
  end
  allowed = arguments_of(index)
  for (key, value) in fields.entries
    return false if not allowed.contains?(key)
    if key == "path"
      path = case value
        Json.Array(items): items
        Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Object(_): return false
      end
      return false if path.size > saved_depth()
      for part in path
        case part
          Json.String(_): Unit
          Json.Number(_): return false if whole(part) == None
          Json.Null | Json.Boolean(_) | Json.Array(_) | Json.Object(_): return false
        end
      end
    else if index == 7 and key == "kind"
      return false if value != Json.String("state") and value != Json.String("failure")
    else if index == 7 and key == "export"
      id = case value
        Json.String(text): text
        Json.Null | Json.Boolean(_) | Json.Number(_) | Json.Array(_) | Json.Object(_): return false
      end
      return false if id.chars.size != 32
      for char in id.chars
        return false if not "0123456789abcdef".chars.contains?(char)
      end
    else if whole(value) == None
      return false
    end
  end
  if index == 7
    return false if fields.get("kind") != Some(Json.String("state")) and
      fields.get("kind") != Some(Json.String("failure"))
    if fields.get("kind") == Some(Json.String("failure"))
      return false if (argument(fields, "failure") or 0) <= 0
    else
      return false if fields.get("failure") != None
    end
    offset = argument(fields, "offset") or 0
    return false if offset % 8192 != 0 or offset >= 17825792
    return false if fields.get("export") == None and offset != 0
  end
  true
end

## The first timer the native side copies; validated arguments only.
fn offset(arguments: Json.Json) : Int
  case arguments
    Json.Object(fields): argument(fields, "offset") or 0
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): 0
  end
end

fn message(item: Message) : Json.Json
  Json.Object({
    "handler": Json.String(handler_name(item.handler)),
    "timer": Json.Boolean(item.timer),
    "arrival_us": Json.String(item.arrival.to_text),
    "sequence": Json.String(item.number),
    "argument_bytes": if item.size_available: Json.String(item.argument_bytes.to_text) else: Json
      .Null,
    "size_unavailable_reason": if item.size_available: Json.Null else: Json.String(
      "arguments were not encoded at admission"
    )
  })
end

fn render(index: Int, arguments: Json.Json, observation: Observation, saved: Bytes, budget: Int) : Json
  .Json
  stats = observation.stats
  fields = case arguments
    Json.Object(value): value
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): stop "validated view arguments"
  end
  start = argument(fields, "offset") or 0
  after = argument(fields, "after")
  before = argument(fields, "before")
  if index == 0
    return Json.Object({
      "service": Json.String(observation.service_name),
      "program_identity": Json.String(observation.program),
      "runtime_identity": if observation.build == "": Json.Null else: Json.String(observation.build),
      "runtime_build": Json.String(observation.build_label),
      "observed_elapsed_us": Json.String(stats.elapsed),
      "uptime_us": Json.String(observation.uptime),
      "running": Json.Boolean(stats.running),
      "mailbox_count": Json.int(observation.mailbox.size),
      "timer_count": Json.int(stats.timer_count),
      "heap_used": Json.String(stats.heap_used.to_text),
      "heap_peak": Json.String(stats.heap_peak.to_text),
      "failures": Json.int(stats.failures),
      "failure_window_us": Json.String(stats.failure_window.to_text),
      "restart_window_us": Json.String(stats.restart_window.to_text),
      "restarts": Json.int(stats.restarts),
      "recovery_sequence": Json.String(stats.sequence.to_text),
      "snapshot_sequence": Json.String(stats.snapshot.to_text),
      "encoded_sequence": Json.String(stats.encoded_sequence.to_text),
      "retained_failures": if stats.retained_failures < 0: Json.Null else: Json.int(
        stats.retained_failures
      ),
      "evicted_kinds_since_open": if stats.retained_failures < 0: Json.Null else: Json.String(
        stats.evicted_kinds
      ),
      "failure_capture_available": Json.Boolean(stats.persistent and stats.retained_failures >= 0),
      "export_available": Json.Boolean(true),
      "observation": Json.String("independent calls are not an atomic snapshot")
    })
  else if index == 1
    return state(saved, fields.get("path") or Json.Array([]), budget, stats.encoded_sequence)
  else if index == 4
    return Json.Object({
      "heap_used": Json.String(stats.heap_used.to_text),
      "heap_peak": Json.String(stats.heap_peak.to_text),
      "heap_limit": Json.String(stats.heap_limit.to_text),
      "stack_capacity": Json.String(stats.stack.to_text),
      "stack_limit": Json.String(stats.stack_limit.to_text),
      "encoded_state_bytes": Json.String(stats.encoded_bytes.to_text),
      "pinned_export_bytes": Json.int(stats.pinned_export_bytes),
      "diagnostic_scratch_peak": Json.String(stats.scratch_peak.to_text),
      "export_available": Json.Boolean(true)
    })
  end
  var items = []
  var numbers = []
  if index == 2
    items = observation.mailbox.drop(start).map(fn(item) message(item) end)
  else if index == 3
    items = observation.timers.map(
      fn(item) Json.Object({
        "name": Json.String(handler_name(item.name)),
        "interval_us": Json.String(item.interval),
        "next_due_elapsed_us": Json.String(item.next),
        "waiting": Json.Boolean(item.waiting)
      }) end
    )
  else if index == 5
    events = observation.events.reverse.filter(fn(item) within(item.number, after, before) end)
    numbers = events.map(fn(item) item.number end)
    items = events.map(
      fn(item) Json.Object({
        "number": Json.String(item.number),
        "kind": Json.String(item.kind),
        "reason": Json.String(item.reason),
        "handler": Json.String(handler_name(item.handler)),
        "arrival_us": Json.String(item.arrival.to_text),
        "elapsed_us": Json.String(item.elapsed),
        "record": if item.record == "0": Json.Null else: Json.String(item.record),
        "record_unavailable": if item.unavailable == "": Json.Null else: Json.String(
          item.unavailable
        )
      }) end
    )
  else
    log = observation.log.filter(fn(item) within(item.number, after, None) end)
    numbers = log.map(fn(item) item.number end)
    items = log.map(fn(item) message(item) end)
  end
  var included = []
  var used = 256
  for item in items
    size = item.to_text.byte_count + 1
    break if used + size > budget
    included = included.append(item)
    used += size
  end
  remaining = if index == 3: (stats.timer_count - start).max(0) else: items.size
  omitted = remaining - included.size
  var result = {
    "items": Json.Array(included),
    "complete": Json.Boolean(omitted == 0),
    "snapshot_sequence": Json.String(stats.snapshot.to_text),
    "retention": Json.String(
      if index == 6: "committed messages since the last snapshot only" else: "current in-memory observation; independent calls are not atomic"
    )
  }
  if omitted > 0
    result = result.set("truncated", Json.int(omitted))
    var next = {}
    if index == 2 or index == 3
      next = next.set("offset", Json.int(start + included.size))
    else
      case (numbers.get(included.size - 1) or "").to_int
        Some(last):
          next = next.set(if index == 5: "before" else: "after", Json.int(last))
          if index == 5
            case after
              Some(bound): next = next.set("after", Json.int(bound))
              None: Unit
            end
          end
        None: Unit
      end
    end
    if not next.empty?
      result = result.set("next", Json.Object(next))
    end
  end
  Json.Object(result)
end

struct SavedGroup
  name: Text
  unnamed: Bool
  fields: List((Text, Int))
end

struct SavedType
  tag: Int
  name: Text
  parameters: List(Int)
  groups: List(SavedGroup)
end

## None when the budget ran out before this value; next is -1 once rendering stopped inside it.
struct Rendered
  value: Option(Json.Json)
  next: Int
  left: Int
  complete: Bool
end

fn octet(bytes: Bytes, at: Int) : Option(Int)
  bytes.get(at).map(fn(value) value.to_int end)
end

fn integer(bytes: Bytes, at: Int, width: Int) : Option(Int)
  return None if at < 0 or width > bytes.size - at
  var value = 0
  for i in 0..width
    value = value.wrapping_mul(256).wrapping_add(try octet(bytes, at + width - i - 1))
  end
  Some(value)
end

fn text(bytes: Bytes, at: Int, size: Int) : Option(Text)
  return None if at < 0 or size < 0 or size > bytes.size - at
  var chars = []
  var offset = at
  while offset < at + size
    first = try octet(bytes, offset)
    width = if first < 128: 1 else if first < 224: 2 else if first < 240: 3 else: 4
    return None if width > at + size - offset
    var code = first % (if width == 1: 128 else if width == 2: 32 else if width == 3: 16 else: 8)
    for j in 1..width
      tail = try octet(bytes, offset + j)
      return None if tail < 128 or tail >= 192
      code = code * 64 + tail - 128
    end
    chars = chars.append(try code.to_char)
    offset += width
  end
  Some(Text.from_chars(chars))
end

fn named(bytes: Bytes, at: Int) : Option((Text, Int))
  size = try integer(bytes, at, 4)
  value = try text(bytes, at + 4, size)
  Some((value, at + 4 + size))
end

fn schema(bytes: Bytes, at: Int, types: List(SavedType), depth: Int) : Option(
  (Int, Int, List(SavedType))
)
  return None if depth > saved_depth()
  tag = try octet(bytes, at)
  if tag == 255
    index = try integer(bytes, at + 1, 4)
    return None if index >= types.size
    return Some((index, at + 5, types))
  end
  return None if tag > 18
  index = types.size
  var all = types.append(SavedType(tag: tag, name: "", parameters: [], groups: []))
  var offset = at + 1
  var name = ""
  if tag == 17 or tag == 18
    (value, next) = try named(bytes, offset)
    name = value
    offset = next
  end
  count = try integer(bytes, offset, 4)
  offset += 4
  var parameters = []
  for _ in 0..count
    (child, next, updated) = try schema(bytes, offset, all, depth + 1)
    parameters = parameters.append(child)
    offset = next
    all = updated
  end
  groups_count = try integer(bytes, offset, 4)
  offset += 4
  var groups = []
  for _ in 0..groups_count
    var group_name = ""
    var unnamed = false
    if tag == 18
      (value, next) = try named(bytes, offset)
      group_name = value
      unnamed = (try octet(bytes, next)) != 0
      offset = next + 1
    end
    fields_count = try integer(bytes, offset, 4)
    offset += 4
    var fields = []
    for _ in 0..fields_count
      (field_name, next) = try named(bytes, offset)
      (child, finish, updated) = try schema(bytes, next, all, depth + 1)
      fields = fields.append((field_name, child))
      all = updated
      offset = finish
    end
    groups = groups.append(SavedGroup(name: group_name, unnamed: unnamed, fields: fields))
  end
  all = try all.set(index, SavedType(tag: tag, name: name, parameters: parameters, groups: groups))
  Some((index, offset, all))
end

fn stopped(left: Int) : Rendered
  Rendered(value: None, next: -1, left: left, complete: false)
end

fn omitted_bytes(size: Int, next: Int, left: Int) : Rendered
  marker = Json.Object({"truncated_bytes": Json.int(size)})
  cost = marker.to_text.byte_count
  return stopped(left) if cost > left
  Rendered(value: Some(marker), next: next, left: left - cost, complete: false)
end

fn big(bytes: Bytes, at: Int, size: Int) : Option(BigInt)
  return None if at < 0 or size > bytes.size - at
  var value = 0.to_big
  for i in 0..size
    value = value * 256.to_big + (try octet(bytes, at + size - 1 - i)).to_big
  end
  Some(value)
end

fn decimal_text(value: BigInt) : Text
  negative = value < 0.to_big
  digits = value.abs.to_text
  padded = "#{"0".repeat((19 - digits.byte_count).max(0))}#{digits}"
  chars = padded.chars
  whole = Text.from_chars(chars.take(chars.size - 18))
  var fraction = chars.drop(chars.size - 18)
  while fraction.last == Some('0')
    fraction = fraction.take(fraction.size - 1)
  end
  sign = if negative: "-" else: ""
  if fraction.empty?: "#{sign}#{whole}" else: "#{sign}#{whole}.#{Text.from_chars(fraction)}"
end

fn scalar(bytes: Bytes, at: Int, tag: Int, budget: Int) : Option(Rendered)
  var next = at
  var value = Json.Null
  var size = -1
  if tag == 0
    Unit
  else if tag == 1 or tag == 2
    n = try octet(bytes, at)
    value = if tag == 1: Json.Boolean(n != 0) else: Json.String(n.to_text)
    next += 1
  else if tag == 3 or tag == 6 or tag == 7 or tag == 9
    n = try integer(bytes, at, 8)
    next += 8
    value = Json.String(n.to_text)
    if tag == 6
      epoch = Time.parse("1970-01-01T00:00:00Z") or stop "epoch"
      micro = (Time.parse("1970-01-01T00:00:00.000001Z") or stop "microsecond") - epoch
      value = Json.String((try epoch.checked_add(micro * n)).to_text)
    else if tag == 9
      finish = try integer(bytes, next, 8)
      next += 8
      value = Json.Object({"start": value, "end": Json.String(finish.to_text)})
    end
  else if tag == 4
    low = try integer(bytes, at, 6)
    upper = try octet(bytes, at + 6)
    last = try octet(bytes, at + 7)
    exponent = (last % 128) * 16 + upper / 16
    mantissa = low + (upper % 16) * 281474976710656
    negative = last >= 128
    if exponent == 2047
      value = Json.String(if mantissa != 0: "NaN" else if negative: "-Infinity" else: "Infinity")
    else
      var number = mantissa.to_float
      if exponent != 0
        number += 4503599627370496.to_float
      end
      power = (if exponent == 0: 1 else: exponent) - 1075
      for _ in 0..power.abs
        number = if power < 0: number / 2.to_float else: number * 2.to_float
      end
      if negative
        number = -number
      end
      value = try Json.float(number)
    end
    next += 8
  else if tag == 5
    code = try integer(bytes, at, 4)
    value = Json.String((try code.to_char).to_text)
    next += 4
  else if tag == 8
    var number = try big(bytes, at, 16)
    if (try octet(bytes, at + 15)) >= 128
      number -= 256.to_big.pow(16)
    end
    value = Json.String(decimal_text(number))
    next += 16
  else if tag == 10 or tag == 11
    size = try integer(bytes, at, 4)
    return None if size > bytes.size - at - 4
    next = at + 4 + size
    least = if tag == 10: size + 2 else: (size + 2) / 3 * 4 + 13
    return Some(omitted_bytes(size, next, budget)) if least > budget
    if tag == 10
      value = Json.String(try text(bytes, at + 4, size))
    else
      alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".chars
      var encoded = []
      var offset = 0
      while offset < size
        a = try octet(bytes, at + 4 + offset)
        b = if offset + 1 < size: try octet(bytes, at + 5 + offset) else: 0
        c = if offset + 2 < size: try octet(bytes, at + 6 + offset) else: 0
        for index in [a / 4, (a % 4) * 16 + b / 16]
          encoded = encoded.append(alphabet.get(index) or stop "base64")
        end
        encoded = encoded.append(
          if offset + 1 < size: alphabet.get((b % 16) * 4 + c / 64) or stop "base64" else: '='
        )
        encoded = encoded.append(
          if offset + 2 < size: alphabet.get(c % 64) or stop "base64" else: '='
        )
        offset += 3
      end
      value = Json.Object({"base64": Json.String(Text.from_chars(encoded))})
    end
  else if tag == 12
    sign = try octet(bytes, at)
    size = try integer(bytes, at + 1, 4)
    return None if size > bytes.size - at - 5
    next = at + 5 + size
    return Some(omitted_bytes(size, next, budget)) if size * 5 / 2 + 3 > budget
    number = try big(bytes, at + 5, size)
    value = Json.String((if sign == 2: -number else: number).to_text)
  else
    return None
  end
  used = value.to_text.byte_count
  if used <= budget
    Some(Rendered(value: Some(value), next: next, left: budget - used, complete: true))
  else if size >= 0
    Some(omitted_bytes(size, next, budget))
  else
    Some(stopped(budget))
  end
end

fn child(kind: SavedType, group: SavedGroup, i: Int) : Option(Int)
  if kind.tag == 13: kind.parameters.get(i) else if kind.tag >= 17: group.fields.get(i).map(
    fn(field) field_type(field) end
  ) else: kind.parameters.get(0)
end

## A struct saves as a choice with one case named after its type.
fn struct_like(kind: SavedType, group: SavedGroup) : Bool
  short = ((kind.name.split("(").first or "").split(".").last or "")
  kind.tag == 17 or (kind.groups.size == 1 and group.name == short)
end

fn walk(bytes: Bytes, at: Int, types: List(SavedType), index: Int, budget: Int, depth: Int) : Option(
  Rendered
)
  return None if depth > saved_depth()
  kind = try types.get(index)
  return scalar(bytes, at, kind.tag, budget) if kind.tag <= 12
  return None if kind.tag > 18
  var offset = at
  var group_index = 0
  if kind.tag == 18
    group_index = try integer(bytes, offset, 4)
    offset += 4
  end
  count = try integer(bytes, offset, 4)
  offset += 4
  group = try kind.groups.get(group_index)
  if kind.tag == 13 or kind.tag >= 17
    expected = if kind.tag == 13: kind.parameters.size else: group.fields.size
    return None if count != expected
  end
  overhead = 128 + (kind.name.byte_count + group.name.byte_count) * 6
  return Some(stopped(budget)) if budget < overhead
  var left = budget - overhead
  var array = []
  var fields = {}
  var complete = true
  var shown = 0
  for i in 0..count
    separator = if kind.tag >= 17: field_name(try group.fields.get(i)).byte_count * 6 + 4 else if kind
      .tag ==
      16: 4 else: 1
    break if separator > left
    left -= separator
    item = if kind.tag == 16: try pair(bytes, offset, types, kind, left, depth + 1) else: try walk(
      bytes,
      offset,
      types,
      try child(kind, group, i),
      left,
      depth + 1
    )
    left = item.left
    break if item.value == None
    value = item.value or stop "rendered value"
    if kind.tag >= 17
      fields = fields.set(field_name(try group.fields.get(i)), value)
    else
      array = array.append(value)
    end
    shown = i + 1
    complete = complete and item.complete
    offset = item.next
    break if offset < 0
  end
  omitted = count - shown
  var value = Json.Null
  if kind.tag >= 17
    var metadata = {"type": Json.String(kind.name)}
    if not struct_like(kind, group)
      metadata = metadata.set("case", Json.String(group.name))
    end
    payload = group.fields.size == 1 and (
      group.unnamed or field_name(try group.fields.get(0)) == "0"
    )
    if payload and shown == 1
      metadata = metadata.set("value", fields.values.first or stop "payload")
    else if not payload and group.fields.size > 0
      metadata = metadata.set("fields", Json.Object(fields))
    end
    if omitted > 0
      metadata = metadata.set("truncated", Json.int(omitted))
    end
    value = Json.Object(metadata)
  else if omitted > 0
    value = Json.Object({"items": Json.Array(array), "truncated": Json.int(omitted)})
  else
    value = Json.Array(array)
  end
  Some(
    Rendered(
      value: Some(value),
      next: if omitted == 0: offset else: -1,
      left: left,
      complete: complete and omitted == 0
    )
  )
end

fn pair(bytes: Bytes, at: Int, types: List(SavedType), kind: SavedType, budget: Int, depth: Int) : Option(
  Rendered
)
  key = try walk(bytes, at, types, try kind.parameters.get(0), budget, depth)
  return Some(stopped(key.left)) if key.value == None or key.next < 0
  value = try walk(bytes, key.next, types, try kind.parameters.get(1), key.left, depth)
  return Some(stopped(value.left)) if value.value == None
  Some(
    Rendered(
      value: Some(Json.Array([key.value or stop "key", value.value or stop "value"])),
      next: value.next,
      left: value.left,
      complete: key.complete and value.complete
    )
  )
end

## Encoded size of every value of a type, or -1 when sizes vary. Fuel bounds shared schema nodes.
fn width(types: List(SavedType), index: Int, fuel: Int) : (Int, Int)
  return (-1, 0) if fuel <= 0
  kind = case types.get(index)
    Some(value): value
    None: return (-1, 0)
  end
  tag = kind.tag
  if tag <= 12
    fixed = if tag == 0: 0 else if tag <= 2: 1 else if tag == 5: 4 else if tag <= 7: 8 else if tag <=
      9: 16 else: -1
    return (fixed, fuel - 1)
  end
  return (-1, fuel - 1) if tag != 13 and tag != 17 and not (tag == 18 and kind.groups.size == 1)
  children = if tag == 13: kind.parameters else: case kind.groups.first
    Some(group): group.fields.map(fn(field) field_type(field) end)
    None: return (-1, fuel - 1)
  end
  var total = if tag == 18: 8 else: 4
  var left = fuel - 1
  for item in children
    (size, rest) = width(types, item, left)
    return (-1, rest) if size < 0
    total += size
    left = rest
  end
  (total, left)
end

fn skip(bytes: Bytes, at: Int, types: List(SavedType), index: Int, depth: Int) : Option(Int)
  return None if depth > saved_depth()
  kind = try types.get(index)
  tag = kind.tag
  if tag == 10 or tag == 11 or tag == 12
    header = if tag == 12: 5 else: 4
    size = try integer(bytes, at + header - 4, 4)
    return None if size > bytes.size - at - header
    return Some(at + header + size)
  end
  if tag < 10
    (size, _fuel) = width(types, index, 1)
    return None if size > bytes.size - at
    return Some(at + size)
  end
  return None if tag > 18
  var offset = at
  var group_index = 0
  if tag == 18
    group_index = try integer(bytes, offset, 4)
    offset += 4
  end
  count = try integer(bytes, offset, 4)
  offset += 4
  group = try kind.groups.get(group_index)
  if tag == 13 or tag >= 17
    return None if count != (if tag == 13: kind.parameters.size else: group.fields.size)
  end
  stride = stride_of(types, kind)
  if stride >= 0
    return None if stride > 0 and count > (bytes.size - offset) / stride
    return Some(offset + count * stride)
  end
  for i in 0..count
    offset = try skip(bytes, offset, types, try child(kind, group, i), depth + 1)
    if tag == 16
      offset = try skip(bytes, offset, types, try kind.parameters.get(1), depth + 1)
    end
  end
  Some(offset)
end

## Encoded size of each element of a list, set or map, or -1.
fn stride_of(types: List(SavedType), kind: SavedType) : Int
  return -1 if kind.tag < 14 or kind.tag > 16
  (first, rest) = width(types, kind.parameters.get(0) or -1, 64)
  return first if kind.tag != 16 or first < 0
  (second, _fuel) = width(types, kind.parameters.get(1) or -1, rest)
  if second < 0: -1 else: first + second
end

fn locate(
  bytes: Bytes,
  at: Int,
  types: List(SavedType),
  index: Int,
  path: List(Json.Json),
  depth: Int
) : Option((Int, Int, Bool))
  return None if depth > saved_depth()
  return Some((at, index, false)) if path.empty?
  kind = try types.get(index)
  return None if kind.tag < 13 or kind.tag > 18
  var offset = at
  var group_index = 0
  if kind.tag == 18
    group_index = try integer(bytes, offset, 4)
    offset += 4
  end
  count = try integer(bytes, offset, 4)
  offset += 4
  group = try kind.groups.get(group_index)
  target = try path.first
  var position = -1
  if kind.tag >= 17
    for i in 0..group.fields.size
      if target == Json.String(field_name(try group.fields.get(i)))
        position = i
        break
      end
    end
  else
    position = whole(target) or -1
  end
  return None if position < 0 or position >= count
  stride = stride_of(types, kind)
  if stride >= 0
    offset += position * stride
    return None if offset > bytes.size
  else
    for i in 0..position
      offset = try skip(bytes, offset, types, try child(kind, group, i), depth + 1)
      if kind.tag == 16
        offset = try skip(bytes, offset, types, try kind.parameters.get(1), depth + 1)
      end
    end
  end
  item = try child(kind, group, position)
  if kind.tag == 16
    return Some((offset, index, true)) if path.size == 1
    part = try path.get(1)
    if part == Json.int(0)
      return locate(bytes, offset, types, item, path.drop(2), depth + 1)
    else if part == Json.int(1)
      return locate(
        bytes,
        try skip(bytes, offset, types, item, depth + 1),
        types,
        try kind.parameters.get(1),
        path.drop(2),
        depth + 1
      )
    end
    return None
  end
  locate(bytes, offset, types, item, path.drop(1), depth + 1)
end

fn state(saved: Bytes, path: Json.Json, budget: Int, sequence: Int) : Json.Json
  case saved_state(saved, path, budget)
    Some(value): Json.Object({
      "state": value.value or Json.Null,
      "complete": Json.Boolean(value.complete),
      "encoded_sequence": Json.String(sequence.to_text),
      "export_available": Json.Boolean(true),
      "advice": Json.String(
        if value.complete: "independent observations are not an atomic snapshot" else: "follow a truncated part with a path or use mo-export for complete saved bytes"
      )
    })
    None: Json.Object({
      "complete": Json.Boolean(false),
      "unavailable": Json.String(
        if saved.size == 0: "encoded state unavailable in this service mode" else: "path not found or saved encoding unavailable"
      )
    })
  end
end

fn saved_state(saved: Bytes, path: Json.Json, budget: Int) : Option(Rendered)
  return None if saved.size < 9 or (try text(saved, 0, 4)) != "MOSV" or (try octet(saved, 4)) != 1
  size = try integer(saved, 5, 4)
  return None if size > saved.size - 9
  (root, next, types) = try schema(saved, 9, [], 0)
  return None if next != 9 + size
  parts = case path
    Json.Array(items): items
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Object(_): return None
  end
  (at, index, is_pair) = try locate(saved, next, types, root, parts, 0)
  if is_pair: pair(saved, at, types, try types.get(index), budget, 0) else: walk(
    saved,
    at,
    types,
    index,
    budget,
    0
  )
end

## A whole saved value for the offline commands, which have no program: its JSON text, then
## whether it is complete, as `{"value":...,"complete":...}`. Past budget bytes a value is marked
## truncated, as the live views mark it. Empty when the bytes are not a saved value.
fn offline_value(saved: Bytes, budget: Int) : Text
  case saved_state(saved, Json.Array([]), budget)
    Some(rendered): Json.Object({
      "value": rendered.value or Json.Null,
      "complete": Json.Boolean(rendered.complete and rendered.value is Some(_))
    }).to_text
    None: ""
  end
end

fn field_type(field: (Text, Int)) : Int
  (_name, index) = field
  index
end

fn field_name(field: (Text, Int)) : Text
  (name, _index) = field
  name
end

fn above(number: Text, bound: Int) : Bool
  threshold = bound.to_text
  number.byte_count > threshold.byte_count or (
    number.byte_count == threshold.byte_count and number > threshold
  )
end

fn below(number: Text, bound: Int) : Bool
  threshold = bound.to_text
  number.byte_count < threshold.byte_count or (
    number.byte_count == threshold.byte_count and number < threshold
  )
end

fn within(number: Text, after: Option(Int), before: Option(Int)) : Bool
  low = case after
    Some(bound): above(number, bound)
    None: true
  end
  high = case before
    Some(bound): below(number, bound)
    None: true
  end
  low and high
end

fn handler_name(name: Text) : Text
  plain = name.split("@").first or ""
  plain.split(".").last or ""
end

fn export_id(arguments: Json.Json) : Text
  case arguments
    Json.Object(fields):
      case fields.get("export")
        Some(Json.String(value)): value
        Some(_) | None: ""
      end
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): ""
  end
end

fn export_reply(error: Text, metadata: List(Text), bytes: Bytes) : Json.Json
  if error != ""
    return Json.Object({"error": Json.String(error), "state_effect": Json.String("none")})
  end
  alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".chars
  var encoded = []
  var at = 0
  while at < bytes.size
    a = (bytes.get(at) or stop "page byte").to_int
    b = if at + 1 < bytes.size: (bytes.get(at + 1) or stop "page byte").to_int else: 0
    c = if at + 2 < bytes.size: (bytes.get(at + 2) or stop "page byte").to_int else: 0
    encoded = encoded.append(alphabet.get(a / 4) or stop "base64")
    encoded = encoded.append(alphabet.get((a % 4) * 16 + b / 16) or stop "base64")
    encoded = encoded.append(
      if at + 1 < bytes.size: alphabet.get((b % 16) * 4 + c / 64) or stop "base64" else: '='
    )
    encoded = encoded.append(
      if at + 2 < bytes.size: alphabet.get(c % 64) or stop "base64" else: '='
    )
    at += 3
  end
  id = metadata.get(0) or stop "export id"
  offset = (metadata.get(3) or stop "offset").to_int or stop "offset"
  digest = metadata.get(7) or stop "digest"
  var next = {
    "kind": Json.String(metadata.get(8) or "state"),
    "export": Json.String(id),
    "offset": Json.int(offset + bytes.size)
  }
  if metadata.get(8) == Some("failure")
    next = next.set(
      "failure",
      Json.int((metadata.get(9) or stop "failure id").to_int or stop "failure id")
    )
  end
  Json.Object({
    "export": Json.String(id),
    "kind": Json.String(metadata.get(8) or "state"),
    "failure": if metadata.get(8) == Some("failure"): Json.int((metadata.get(9) or "0").to_int or 0) else: Json
      .Null,
    "version": Json.int(1),
    "sequence": Json.String(metadata.get(1) or stop "sequence"),
    "encoded_sequence": Json.String(metadata.get(2) or stop "encoded sequence"),
    "offset": Json.int(offset),
    "length": Json.int(bytes.size),
    "total": Json.int((metadata.get(4) or stop "total").to_int or stop "total"),
    "pages": Json.int((metadata.get(5) or stop "pages").to_int or stop "pages"),
    "checksum": Json.String(metadata.get(6) or stop "checksum"),
    "digest": if digest == "": Json.Null else: Json.String(digest),
    "base64": Json.String(Text.from_chars(encoded)),
    "next": if digest != "": Json.Null else: Json.Object(next)
  })
end

fn failure_id(arguments: Json.Json) : Int
  case arguments
    Json.Object(fields): argument(fields, "failure") or 0
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): 0
  end
end

fn failure_after(arguments: Json.Json) : Int
  case arguments
    Json.Object(fields): argument(fields, "after") or 0
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): 0
  end
end

fn failure_reply(arguments: Json.Json, metadata: List(Text)) : Json.Json
  after = failure_after(arguments)
  var rows = []
  var unavailable = []
  var unavailable_count = 0
  var i = 0
  var last = after
  var omitted = 0
  var used = 1024
  while i + 14 < metadata.size
    id = (metadata.get(i) or "0").to_int or 0
    if id == -1
      omitted += (metadata.get(i + 1) or "0").to_int or 0
    end
    if id == 0
      unavailable_count += 1
    end
    if id == 0 and after == 0 and unavailable.size < 8
      unavailable = unavailable.append(
        Json.Object({
          "reason": Json.String(metadata.get(i + 1) or ""),
          "arrival": Json.String(metadata.get(i + 3) or "")
        })
      )
    end
    if id > after
      row = Json.Object({
        "id": Json.String(id.to_text),
        "kind": Json.String(metadata.get(i + 1) or ""),
        "handler": Json.String(metadata.get(i + 2) or ""),
        "arrival": Json.String(metadata.get(i + 3) or ""),
        "state_bytes": Json.String(metadata.get(i + 4) or ""),
        "message_bytes": Json.String(metadata.get(i + 5) or ""),
        "signature": Json.Object({
          "version": Json.int((metadata.get(i + 6) or "0").to_int or 0),
          "class": Json.String(metadata.get(i + 7) or ""),
          "reason": Json.String(metadata.get(i + 8) or ""),
          "limit": Json.String(metadata.get(i + 9) or ""),
          "value": Json.String(metadata.get(i + 10) or ""),
          "function": Json.String(metadata.get(i + 11) or ""),
          "file": Json.String(metadata.get(i + 12) or ""),
          "start": Json.String(metadata.get(i + 13) or ""),
          "end": Json.String(metadata.get(i + 14) or "")
        })
      })
      size = row.to_text.byte_count + 1
      if omitted == 0 and used + size <= 6144
        rows = rows.append(row)
        last = id
        used += size
      else
        omitted += 1
      end
    end
    i += 15
  end
  Json.Object({
    "records": Json.Array(rows),
    "unavailable": Json.Array(unavailable),
    "unavailable_omitted": Json.int(unavailable_count - unavailable.size),
    "truncated": Json.int(omitted),
    "next": if omitted > 0: Json.Object({"after": Json.int(last)}) else: Json.Null
  })
end

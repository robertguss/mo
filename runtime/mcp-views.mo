module McpViews
expose names, catalog, valid, render, Observation(..), Stats(..), Message(..), Timer(..), Event(..)
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
  ["mo-overview", "mo-state", "mo-mailbox", "mo-timers", "mo-memory", "mo-supervisor", "mo-log"]
end

fn catalog() : List(Json.Json)
  descriptions = [
    "Runtime identities, recovery position and live counters. Independent observations are not an atomic snapshot. Retained failure records and export are not available.",
    "Current saved state, including private fields. Optional path contains field names or nonnegative indexes. Large views explicitly truncate; export is not yet available.",
    "Queued handler names, arrival times and encoded argument sizes where available, never argument values. Does not execute handlers.",
    "Pending timer names, intervals, next due elapsed times and waiting ticks. Does not execute handlers.",
    "Live heap use, peak and limit, stack capacity and encoded state bytes. Export pins and retained diagnostics are not yet available.",
    "Retained in-memory supervisor events, newest first, optionally after an event number. Events are lost on process exit; failure capture is not yet available.",
    "Committed message metadata since the last snapshot, optionally after a sequence. Older successful messages are not retained. Does not execute handlers."
  ]
  tool_names = names()
  var tools = []
  for i in 0..tool_names.size
    var properties = {}
    if i == 1
      properties = properties.set(
        "path",
        Json.Object({
          "type": Json.String("array"),
          "maxItems": Json.int(128),
          "items": Json.Object({
            "anyOf": Json.Array([
              Json.Object({"type": Json.String("string")}),
              Json.Object({"type": Json.String("integer"), "minimum": Json.int(0)})
            ])
          })
        })
      )
    else if i == 5 or i == 6
      properties = properties.set(
        "after",
        Json.Object({"type": Json.String("integer"), "minimum": Json.int(0)})
      )
    end
    tools = tools.append(
      Json.Object({
        "name": Json.String(tool_names.get(i) or stop "view name"),
        "description": Json.String(descriptions.get(i) or stop "view description"),
        "inputSchema": Json.Object({
          "type": Json.String("object"),
          "properties": Json.Object(properties),
          "additionalProperties": Json.Boolean(false)
        }),
        "annotations": Json.Object({
          "readOnlyHint": Json.Boolean(true),
          "idempotentHint": Json.Boolean(true)
        })
      })
    )
  end
  tools
end

fn valid(index: Int, arguments: Json.Json) : Bool
  fields = case arguments
    Json.Object(value): value
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return false
  end
  for (key, value) in fields.entries
    if index == 1 and key == "path"
      path = case value
        Json.Array(items): items
        Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Object(_): return false
      end
      return false if path.size > 128
      for part in path
        case part
          Json.String(_): Unit
          Json.Number(_):
            number = part.to_text.to_int or -1
            return false if number < 0
          Json.Null | Json.Boolean(_) | Json.Array(_) | Json.Object(_): return false
        end
      end
    else if (index == 5 or index == 6) and key == "after"
      case value
        Json.Number(_): return false if (value.to_text.to_int or -1) < 0
        Json.Null | Json.Boolean(_) | Json.String(_) | Json.Array(_) | Json.Object(_): return false
      end
    else
      return false
    end
  end
  true
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
  after = (fields.get("after") or Json.int(0)).to_text.to_int or 0
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
      "retained_failures": Json.Null,
      "failure_capture_available": Json.Boolean(false),
      "export_available": Json.Boolean(false),
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
      "pinned_export_bytes": Json.Null,
      "diagnostic_scratch_peak": Json.String(stats.scratch_peak.to_text),
      "export_available": Json.Boolean(false)
    })
  end
  var items = []
  if index == 2
    items = observation.mailbox.map(fn(item) message(item) end)
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
    items = observation.events.reverse.filter(fn(item) after_number(item.number, after) end).map(fn(
      item
    )
      Json.Object({
        "number": Json.String(item.number),
        "kind": Json.String(item.kind),
        "reason": Json.String(item.reason),
        "handler": Json.String(handler_name(item.handler)),
        "arrival_us": Json.String(item.arrival.to_text),
        "elapsed_us": Json.String(item.elapsed)
      })
    end)
  else
    items = observation.log.filter(fn(item) after_number(item.number, after) end).map(
      fn(item) message(item) end
    )
  end
  var included = []
  var used = 256
  for item in items
    size = item.to_text.byte_count + 1
    break if used + size > budget
    included = included.append(item)
    used += size
  end
  omitted = items.size -
    included.size + (if index == 3: stats.timer_count - observation.timers.size else: 0)
  if omitted > 0
    included = included.append(Json.Object({"truncated": Json.int(omitted)}))
  end
  Json.Object({
    "items": Json.Array(included),
    "complete": Json.Boolean(omitted == 0),
    "snapshot_sequence": Json.String(stats.snapshot.to_text),
    "retention": Json.String(
      if index == 6: "committed messages since the last snapshot only" else: "current in-memory observation; independent calls are not atomic"
    )
  })
end

struct SavedGroup
  name: Text
  fields: List((Text, Int))
end

struct SavedType
  tag: Int
  name: Text
  parameters: List(Int)
  groups: List(SavedGroup)
end

struct Rendered
  value: Json.Json
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
  return None if depth > 128
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
    if tag == 18
      (value, next) = try named(bytes, offset)
      group_name = value
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
    groups = groups.append(SavedGroup(name: group_name, fields: fields))
  end
  all = try all.set(index, SavedType(tag: tag, name: name, parameters: parameters, groups: groups))
  Some((index, offset, all))
end

fn cut(count: Int, left: Int) : Rendered
  Rendered(
    value: Json.Object({"truncated": Json.int(count)}),
    next: -1,
    left: left,
    complete: false
  )
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
    return Some(cut(size, budget)) if budget >= 0 and size * 6 + 32 > budget
    if budget >= 0
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
    end
    next = at + 4 + size
  else if tag == 12
    sign = try octet(bytes, at)
    size = try integer(bytes, at + 1, 4)
    return None if size > bytes.size - at - 5
    return Some(cut(size, budget)) if budget >= 0 and size * 3 + 32 > budget
    if budget >= 0
      number = try big(bytes, at + 5, size)
      value = Json.String((if sign == 2: -number else: number).to_text)
    end
    next = at + 5 + size
  else
    return None
  end
  if budget < 0
    return Some(Rendered(value: Json.Null, next: next, left: budget, complete: true))
  end
  used = value.to_text.byte_count
  if used > budget
    Some(cut(1, budget))
  else
    Some(Rendered(value: value, next: next, left: budget - used, complete: true))
  end
end

fn walk(bytes: Bytes, at: Int, types: List(SavedType), index: Int, budget: Int, depth: Int) : Option(
  Rendered
)
  return None if depth > 128
  kind = try types.get(index)
  return scalar(bytes, at, kind.tag, budget) if kind.tag <= 12
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
  return Some(cut(count, budget)) if budget >= 0 and
    budget < 128 + (kind.name.byte_count + group.name.byte_count) * 6
  var left = if budget < 0: budget else: budget -
    128 -
    (kind.name.byte_count + group.name.byte_count) * 6
  var array = []
  var fields = {}
  var complete = true
  var omitted_fields = 0
  for i in 0..count
    if budget >= 0 and kind.tag >= 17
      key_cost = field_name(try group.fields.get(i)).byte_count * 6 + 4
      if key_cost > left
        omitted_fields = count - i
        complete = false
        offset = -1
        break
      end
      left -= key_cost
    end
    child = if kind.tag == 13: try kind.parameters.get(i) else if kind.tag >= 17: field_type(
      try group.fields.get(i)
    ) else: try kind.parameters.get(0)
    item = try walk(bytes, offset, types, child, left, depth + 1)
    offset = item.next
    left = item.left
    var value = item.value
    if kind.tag == 16
      second = if item.complete: try walk(
        bytes,
        offset,
        types,
        try kind.parameters.get(1),
        left,
        depth + 1
      ) else: cut(1, left)
      offset = second.next
      left = second.left
      value = Json.Array([value, second.value])
      complete = item.complete and second.complete
    else
      complete = item.complete
    end
    if budget >= 0
      if kind.tag >= 17
        fields = fields.set(field_name(try group.fields.get(i)), value)
      else
        array = array.append(value)
      end
    end
    if not complete
      omitted = count - i - 1
      if kind.tag >= 17
        omitted_fields = omitted
      else if omitted > 0
        array = array.append(Json.Object({"truncated": Json.int(omitted)}))
      end
      break
    end
  end
  var value = Json.Null
  if kind.tag >= 17
    var metadata = if kind.tag == 17: {
      "type": Json.String(kind.name),
      "fields": Json.Object(fields)
    } else: {
      "type": Json.String(kind.name),
      "case": Json.String(group.name),
      "fields": Json.Object(fields)
    }
    if not complete
      metadata = metadata.set("truncated", Json.int(omitted_fields))
    end
    value = Json.Object(metadata)
  else
    value = Json.Array(array)
  end
  Some(Rendered(value: value, next: offset, left: left, complete: complete))
end

fn locate(
  bytes: Bytes,
  at: Int,
  types: List(SavedType),
  index: Int,
  path: List(Json.Json),
  depth: Int
) : Option((Int, Int, Bool))
  return None if depth > 128
  return Some((at, index, false)) if path.empty?
  kind = try types.get(index)
  return None if kind.tag < 13
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
  for i in 0..count
    child = if kind.tag == 13: try kind.parameters.get(i) else if kind.tag >= 17: field_type(
      try group.fields.get(i)
    ) else: try kind.parameters.get(0)
    selected = if kind.tag >= 17: target == Json.String(field_name(try group.fields.get(i))) else: target ==
      Json.int(i)
    if selected
      if kind.tag == 16
        return Some((offset, index, true)) if path.size == 1
        part = try path.get(1)
        if part == Json.int(0)
          return locate(bytes, offset, types, child, path.drop(2), depth + 1)
        else if part == Json.int(1)
          skipped_key = try walk(bytes, offset, types, child, -1, depth + 1)
          return locate(
            bytes,
            skipped_key.next,
            types,
            try kind.parameters.get(1),
            path.drop(2),
            depth + 1
          )
        end
        return None
      end
      return locate(bytes, offset, types, child, path.drop(1), depth + 1)
    end
    skipped = try walk(bytes, offset, types, child, -1, depth + 1)
    offset = skipped.next
    if kind.tag == 16
      second = try walk(bytes, offset, types, try kind.parameters.get(1), -1, depth + 1)
      offset = second.next
    end
  end
  None
end

fn state(saved: Bytes, path: Json.Json, budget: Int, sequence: Int) : Json.Json
  result = saved_state(saved, path, budget)
  case result
    Some(value): Json.Object({
      "state": value.value,
      "complete": Json.Boolean(value.complete),
      "encoded_sequence": Json.String(sequence.to_text),
      "export_available": Json.Boolean(false),
      "advice": Json.String(
        if value.complete: "independent observations are not an atomic snapshot" else: "use a narrower path; full export is not yet available"
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
  (at, index, pair) = try locate(saved, next, types, root, parts, 0)
  if pair
    kind = try types.get(index)
    key = try walk(saved, at, types, try kind.parameters.get(0), budget - 4, 0)
    value = if key.complete: try walk(
      saved,
      key.next,
      types,
      try kind.parameters.get(1),
      key.left,
      0
    ) else: cut(1, key.left)
    return Some(
      Rendered(
        value: Json.Array([key.value, value.value]),
        next: value.next,
        left: value.left,
        complete: key.complete and value.complete
      )
    )
  end
  walk(saved, at, types, index, budget, 0)
end

fn field_type(field: (Text, Int)) : Int
  (_name, index) = field
  index
end

fn field_name(field: (Text, Int)) : Text
  (name, _index) = field
  name
end

fn after_number(number: Text, after: Int) : Bool
  threshold = after.to_text
  number.byte_count > threshold.byte_count or (
    number.byte_count == threshold.byte_count and number > threshold
  )
end

fn handler_name(name: Text) : Text
  plain = name.split("@").first or ""
  plain.split(".").last or ""
end

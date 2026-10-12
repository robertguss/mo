## A value-based Redis-like string store with lazy expiry and checked INCR.
## Quoted command arguments may contain spaces; SET replaces any prior expiry.
module Kv
expose Store, Command(..), Reply(..), CommandError(..), empty, parse, apply, execute

struct Entry
  value: Text
  expires: Option(Time)
end

struct Store
  entries: Map(Text, Entry)
end

enum Command
  SetValue(key: Text, value: Text)
  GetValue(Text)
  DeleteKeys(List(Text))
  Increment(Text)
  ExpireKey(key: Text, seconds: Int)
end

enum CommandError
  BadCommand(Text)
  NotInteger
  IncrementOverflow
  BadExpiry
end

enum Reply
  Simple(Text)
  Bulk(Option(Text))
  IntegerReply(Int)
  FailedReply(CommandError)
end

fn to_text(problem: CommandError) : Text
  case problem
    BadCommand(reason): reason
    NotInteger: "value is not a signed 64-bit integer"
    IncrementOverflow: "increment would overflow a signed 64-bit integer"
    BadExpiry: "expiry is outside the supported time range"
  end
end

fn to_text(reply: Reply) : Text
  case reply
    Simple(text): text
    Bulk(Some(text)): text
    Bulk(None): "(nil)"
    IntegerReply(number): number.to_text
    FailedReply(problem): "ERR #{problem}"
  end
end

fn empty() : Store
  Store(entries: {})
end

fn arguments(line: Text) : Result(List(Text), CommandError)
  var parts = []
  var current = ""
  var started? = false
  var quote = None
  var escaped? = false
  for ch in line.chars
    if escaped?
      piece = case ch
        'n': "\n"
        'r': "\r"
        't': "\t"
        other: other.to_text
      end
      current = "#{current}#{piece}"
      escaped? = false
    else if ch == '\\'
      escaped? = true
      started? = true
    else if quote is Some(delimiter)
      if ch == delimiter
        quote = None
      else
        current = "#{current}#{ch}"
      end
    else if ch == '"' or ch == '\''
      quote = Some(ch)
      started? = true
    else if ch.ascii_white_space?
      if started?
        parts = parts.append(current)
        current = ""
        started? = false
      end
    else
      current = "#{current}#{ch}"
      started? = true
    end
  end
  return Error(BadCommand("unfinished quote or escape")) if escaped? or quote is Some(_)
  if started?
    parts = parts.append(current)
  end
  Ok(parts)
end

fn parse(line: Text) : Result(Command, CommandError)
  parts = try arguments(line)
  first = try parts.first.or_fail(BadCommand("empty command"))
  normalized = [first.ascii_uppercase].concat(parts.drop(1))
  case normalized
    ["SET", key, value]: Ok(SetValue(key: key, value: value))
    ["GET", key]: Ok(GetValue(key))
    ["DEL", first_key, ..remaining]: Ok(DeleteKeys([first_key].concat(remaining)))
    ["INCR", key]: Ok(Increment(key))
    ["EXPIRE", key, duration]:
      seconds = try duration.to_int.or_fail(BadCommand("EXPIRE needs an integer number of seconds"))
      Ok(ExpireKey(key: key, seconds: seconds))
    _: Error(BadCommand("expected SET key value, GET key, DEL key..., INCR key, or EXPIRE key seconds"))
  end
end

fn live(store: Store, now: Time) : Store
  var updated = store
  for (key, entry) in store.entries
    if entry.expires is Some(deadline) and now >= deadline
      updated.entries = updated.entries.remove(key)
    end
  end
  updated
end

fn apply(store: Store, command: Command, now: Time) : (Reply, Store)
  var updated = live(store, now)
  case command
    SetValue(key: key, value: value):
      updated.entries = updated.entries.set(key, Entry(value: value, expires: None))
      (Simple("OK"), updated)
    GetValue(key):
      (Bulk(updated.entries.get(key).map(fn(entry) entry.value end)), updated)
    DeleteKeys(keys):
      var count = 0
      for key in keys
        if updated.entries.has?(key)
          updated.entries = updated.entries.remove(key)
          count += 1
        end
      end
      (IntegerReply(count), updated)
    Increment(key):
      previous = updated.entries.get(key) or Entry(value: "0", expires: None)
      number = case previous.value.to_int
        Some(value): value
        None: return (FailedReply(NotInteger), updated)
      end
      incremented = case number.checked_add(1)
        Some(value): value
        None: return (FailedReply(IncrementOverflow), updated)
      end
      updated.entries = updated.entries.set(key, Entry(value: incremented.to_text, expires: previous.expires))
      (IntegerReply(incremented), updated)
    ExpireKey(key: key, seconds: seconds):
      entry = case updated.entries.get(key)
        Some(value): value
        None: return (IntegerReply(0), updated)
      end
      if seconds <= 0
        updated.entries = updated.entries.remove(key)
        return (IntegerReply(1), updated)
      end
      # The whole Time range is shorter than this many seconds. Check before
      # constructing Duration so an arbitrary Int cannot overflow it.
      return (FailedReply(BadExpiry), updated) if seconds > 315_537_897_599
      deadline = case now.checked_add(seconds.seconds)
        Some(value): value
        None: return (FailedReply(BadExpiry), updated)
      end
      updated.entries = updated.entries.set(key, Entry(value: entry.value, expires: Some(deadline)))
      (IntegerReply(1), updated)
  end
end

fn execute(store: Store, line: Text, now: Time) : (Reply, Store)
  case parse(line)
    Ok(command): apply(store, command, now)
    Error(problem): (FailedReply(problem), store)
  end
end

fn main(console: Console, clock: Clock)
  var store = empty()
  while console.read_line is Some(line)
    (reply, updated) = execute(store, line, clock.now)
    store = updated
    console.print("#{reply}")
  end
end

test "INCR reports overflow without replacing the value, and keeps an existing expiry"
  now = Time.parse("2026-01-01T00:00:00Z") or stop "valid test instant"
  (set_reply, first) = execute(empty(), "SET n 9223372036854775807", now)
  assert set_reply == Simple("OK")
  (overflow_reply, second) = execute(first, "INCR n", now)
  assert overflow_reply == FailedReply(IncrementOverflow)
  assert second == first
  (reset_reply, third) = execute(second, "SET n 3", now)
  assert reset_reply == Simple("OK")
  (expiry_reply, fourth) = execute(third, "EXPIRE n 2", now)
  assert expiry_reply == IntegerReply(1)
  (increment_reply, fifth) = execute(fourth, "INCR n", now + 1.seconds)
  assert increment_reply == IntegerReply(4)
  (get_reply, sixth) = execute(fifth, "GET n", now + 2.seconds)
  assert get_reply == Bulk(None)
  assert sixth.entries.empty?
end

## The core of a Redis-like key-value store. `parse` reads one command, such as
## SET name ada or INCR visits; `apply` applies it to a store at a given time,
## giving the reply and the new store. `main` serves commands from standard
## input, one a line, and prints each reply.
module Kv
expose Store, Command(..), Reply(..), ParseError(..), empty_store, parse, apply

## The longest expiry EXPIRE accepts, in seconds: about 100 years.
max_expiry = 3_153_600_000

struct Item
  value: Text
  expires: Option(Time)
end

struct Store
  entries: Map(Text, Item)
end

enum Command
  Put(key: Text, value: Text)
  Get(key: Text)
  Del(keys: List(Text))
  Incr(key: Text)
  Expire(key: Text, seconds: Int)
end

enum Reply
  Okay
  Value(Text)
  NoValue
  Count(Int)
  Failure(message: Text)
end

enum ParseError
  Empty
  UnknownCommand(name: Text)
  WrongArguments(command: Text)
  NotAnInteger(text: Text)
end

fn to_text(reply: Reply) : Text
  case reply
    Okay: "OK"
    Value(text): "\"#{text}\""
    NoValue: "(nil)"
    Count(n): "(integer) #{n}"
    Failure(message: m): "(error) #{m}"
  end
end

empty_store = Store(entries: {})

fn parse(line: Text) : Result(Command, ParseError)
  examples
    parse("SET name ada") == Ok(Put(key: "name", value: "ada"))
    parse("get name") == Ok(Get(key: "name"))
    parse("DEL a b") == Ok(Del(keys: ["a", "b"]))
    parse("EXPIRE name ten") == Error(NotAnInteger(text: "ten"))
    parse("GET") == Error(WrongArguments(command: "GET"))
    parse("FLY away") == Error(UnknownCommand(name: "FLY"))
    parse("   ") == Error(Empty)
  end

  case line.words
    []: Error(Empty)
    [name, ..args]: build_command(name.ascii_uppercase, args)
  end
end

fn build_command(name: Text, args: List(Text)) : Result(Command, ParseError)
  case (name, args)
    ("SET", [key, value]): Ok(Put(key: key, value: value))
    ("GET", [key]): Ok(Get(key: key))
    ("DEL", [_, .._]): Ok(Del(keys: args))
    ("INCR", [key]): Ok(Incr(key: key))
    ("EXPIRE", [key, seconds]):
      n = try seconds.to_int.or_fail(NotAnInteger(text: seconds))
      Ok(Expire(key: key, seconds: n))
    _ if ["SET", "GET", "DEL", "INCR", "EXPIRE"].contains?(name): Error(
      WrongArguments(command: name)
    )
    _: Error(UnknownCommand(name: name))
  end
end

fn apply(store: Store, command: Command, now: Time) : (Store, Reply)
  promise "a store never holds a key past its expiry"
    result.0.entries.values.all?(fn(entry) entry.expires is Some(at) implies at > now end)
    allows store: empty_store, command: Put(key: "a", value: "1"), now: t0, result: (
      holding("a", "1", None),
      Okay
    )
    forbids store: empty_store, command: Put(key: "a", value: "1"), now: t0, result: (
      holding("a", "1", Some(t0)),
      Okay
    )
  end

  live = without_expired(store, now)
  case command
    Put(key: key, value: value): (put(live, key, Item(value: value, expires: None)), Okay)
    Get(key: key):
      case live.entries.get(key)
        Some(entry): (live, Value(entry.value))
        None: (live, NoValue)
      end
    Del(keys: keys):
      var after = live
      for key in keys
        after.entries = after.entries.remove(key)
      end
      (after, Count(live.entries.size - after.entries.size))
    Incr(key: key): increment(live, key)
    Expire(key: key, seconds: n): expire(live, key, n, now)
  end
end

fn increment(store: Store, key: Text) : (Store, Reply)
  examples
    increment(holding("n", "41", None), "n").1 == Count(42)
    increment(empty_store, "n").1 == Count(1)
    increment(holding("n", "x", None), "n").1 == Failure(message: "the value is not an integer")
    increment(holding("n", "9223372036854775807", None), "n").1 ==
      Failure(message: "the increment would overflow")
  end

  entry = store.entries.get(key) or Item(value: "0", expires: None)
  case entry.value.to_int
    None: (store, Failure(message: "the value is not an integer"))
    Some(n):
      case n.checked_add(1)
        None: (store, Failure(message: "the increment would overflow"))
        Some(more):
          var updated = entry
          updated.value = "#{more}"
          (put(store, key, updated), Count(more))
      end
  end
end

## Sets a key to expire `ttl` seconds from now. A `ttl` of 0 or less removes
## the key at once, as Redis does.
fn expire(store: Store, key: Text, ttl: Int, now: Time) : (Store, Reply)
  examples
    expire(empty_store, "a", 10, t0).1 == Count(0)
    expire(holding("a", "1", None), "a", 10, t0).1 == Count(1)
    expire(holding("a", "1", None), "a", -1, t0) == (empty_store, Count(1))
    expire(holding("a", "1", None), "a", 4_000_000_000, t0).1 ==
      Failure(message: "invalid expire time")
  end

  return (store, Failure(message: "invalid expire time")) if ttl > max_expiry
  case store.entries.get(key)
    None: (store, Count(0))
    Some(_) if ttl <= 0: (Store(entries: store.entries.remove(key)), Count(1))
    Some(entry):
      case now.checked_add(ttl.seconds)
        None: (store, Failure(message: "invalid expire time"))
        Some(at):
          var timed = entry
          timed.expires = Some(at)
          (put(store, key, timed), Count(1))
      end
  end
end

fn without_expired(store: Store, now: Time) : Store
  var kept = store
  for (key, entry) in store.entries
    if entry.expires is Some(at) and at <= now
      kept.entries = kept.entries.remove(key)
    end
  end
  kept
end

fn put(store: Store, key: Text, entry: Item) : Store
  Store(entries: store.entries.set(key, entry))
end

fn main(console: Console, clock: Clock)
  var store = empty_store
  while console.read_line is Some(line)
    reply = case parse(line)
      Ok(command):
        (next, answer) = apply(store, command, clock.now)
        store = next
        answer
      Error(problem): Failure(message: describe(problem))
    end
    console.print("#{reply}")
  end
end

fn describe(problem: ParseError) : Text
  case problem
    Empty: "no command"
    UnknownCommand(name: name): "unknown command #{name}"
    WrongArguments(command: name): "wrong number of arguments for #{name}"
    NotAnInteger(text: text): "#{text} is not an integer"
  end
end

t0 = Time.parse("2026-10-12T09:00:00Z") or stop "the time is written correctly"

fn holding(key: Text, value: Text, expires: Option(Time)) : Store
  Store(entries: {key: Item(value: value, expires: expires)})
end

test "a key expires after its time"
  (with_key, _) = apply(empty_store, Put(key: "session", value: "abc"), t0)
  (timed, set) = apply(with_key, Expire(key: "session", seconds: 10), t0)
  assert set == Count(1)
  (_, before) = apply(timed, Get(key: "session"), t0 + 9.seconds)
  (_, after) = apply(timed, Get(key: "session"), t0 + 10.seconds)
  assert before == Value("abc")
  assert after == NoValue
end

test "the console session prints each reply"
  console = Console.fake_input(["SET a 1", "INCR a", "GET a", "FLY"])
  main(console, Clock.fixed(t0))
  assert console.printed == ["OK", "(integer) 2", "\"2\"", "(error) unknown command FLY"]
end

## A key-value store that serves commands read from standard input, one on each line.
module Store

fn increment(store: Map(Text, Text), key: Text, amount: Text) : (Text, Map(Text, Text))
  delta = case amount.to_int
    Some(n): n
    None: return ("ERR not an integer", store)
  end
  current = case (store.get(key) or "0").to_int
    Some(n): n
    None: return ("ERR not an integer", store)
  end
  case current.checked_add(delta)
    Some(n): (n.to_text, store.set(key, n.to_text))
    None: ("ERR overflow", store)
  end
end

## Applies one command line to the store, giving the reply and the new store.
fn apply(store: Map(Text, Text), line: Text) : (Text, Map(Text, Text))
  examples
    apply({}, "SET a 1") == ("OK", {"a": "1"})
    apply({"a": "1"}, "GET a") == ("1", {"a": "1"})
    apply({}, "GET a") == ("(nil)", {})
    apply({"a": "1"}, "DEL a") == ("1", {})
    apply({}, "PING") == ("ERR unknown command", {})
  end

  case line.words
    ["SET", key, ..words] if not words.empty?: ("OK", store.set(key, words.join(" ")))
    ["GET", key]: (store.get(key) or "(nil)", store)
    ["DEL", key]: if store.has?(key): ("1", store.remove(key)) else: ("0", store)
    ["INCR", key]: increment(store, key, "1")
    ["INCRBY", key, amount]: increment(store, key, amount)
    ["KEYS"]: (if store.empty?: "(empty)" else: store.keys.join(" "), store)
    _: ("ERR unknown command", store)
  end
end

fn main(console: Console)
  var store = {}
  while console.read_line is Some(line)
    (reply, next) = apply(store, line)
    console.print(reply)
    store = next
  end
end

test "commands change the store in turn"
  console = Console.fake_input([
    "SET name Ada Lovelace",
    "GET name",
    "DEL name",
    "GET name",
    "DEL name"
  ])
  main(console)
  assert console.printed == ["OK", "Ada Lovelace", "1", "(nil)", "0"]
end

test "increments create values and store their results as text"
  console = Console.fake_input([
    "INCR count",
    "GET count",
    "INCRBY count -7",
    "INCR count",
    "GET count",
    "INCRBY other 12",
    "GET other"
  ])
  main(console)
  assert console.printed == ["1", "1", "-6", "-5", "-5", "12", "12"]
end

test "keys keep insertion order across updates and move to the end after deletion"
  console = Console.fake_input([
    "KEYS",
    "SET zebra 3",
    "INCR apple",
    "INCRBY middle -2",
    "SET zebra 9",
    "INCR apple",
    "KEYS",
    "DEL zebra",
    "SET zebra 0",
    "KEYS",
    "DEL apple",
    "DEL middle",
    "DEL zebra",
    "KEYS"
  ])
  main(console)
  assert console.printed == [
    "(empty)",
    "OK",
    "1",
    "-2",
    "OK",
    "2",
    "zebra apple middle",
    "1",
    "OK",
    "apple middle zebra",
    "1",
    "1",
    "1",
    "(empty)"
  ]
end

test "invalid integers leave existing values and missing keys unchanged"
  for invalid in [
    "+1",
    "-",
    "--1",
    "1.5",
    "1e2",
    "0x10",
    "1_0",
    "１２",
    "١",
    "9223372036854775808",
    "-9223372036854775809"
  ]
    store = {"zebra": "7", "bad": invalid, "apple": "9"}
    assert apply(store, "INCR bad") == ("ERR not an integer", store)
    assert apply(store, "INCRBY bad 2") == ("ERR not an integer", store)
    assert apply(store, "INCRBY zebra #{invalid}") == ("ERR not an integer", store)
    assert apply(store, "INCRBY absent #{invalid}") == ("ERR not an integer", store)
    (reply, unchanged) = apply(store, "INCRBY absent #{invalid}")
    assert reply == "ERR not an integer"
    assert apply(unchanged, "KEYS").0 == "zebra bad apple"
  end
  for invalid in ["", " 2", "2 ", "1 2"]
    store = {"bad": invalid}
    assert apply(store, "INCR bad") == ("ERR not an integer", store)
    assert apply(store, "INCRBY bad 0") == ("ERR not an integer", store)
  end
end

test "increments accept both integer limits and reject overflowing results without mutation"
  high = {"count": "9223372036854775807", "other": "5"}
  low = {"count": "-9223372036854775808", "other": "5"}
  assert apply(high, "INCR count") == ("ERR overflow", high)
  assert apply(high, "INCRBY count 1") == ("ERR overflow", high)
  assert apply(low, "INCRBY count -1") == ("ERR overflow", low)
  assert apply(high, "INCRBY count 9223372036854775807") == ("ERR overflow", high)
  assert apply(low, "INCRBY count -9223372036854775808") == ("ERR overflow", low)
  assert apply({"count": "9223372036854775806"}, "INCR count") == (
    "9223372036854775807",
    {"count": "9223372036854775807"}
  )
  assert apply(low, "INCR count") == (
    "-9223372036854775807",
    {"count": "-9223372036854775807", "other": "5"}
  )
  assert apply(high, "INCRBY count -9223372036854775808") == ("-1", {"count": "-1", "other": "5"})
  assert apply(low, "INCRBY count 9223372036854775807") == ("-1", {"count": "-1", "other": "5"})
  assert apply({}, "INCRBY count -9223372036854775808") == (
    "-9223372036854775808",
    {"count": "-9223372036854775808"}
  )
  assert apply({}, "INCRBY count 9223372036854775807") == (
    "9223372036854775807",
    {"count": "9223372036854775807"}
  )
end

test "increments normalize integer text and commands accept whitespace"
  assert apply({"count": "0009"}, "INCR count") == ("10", {"count": "10"})
  assert apply({"count": "-0002"}, "INCRBY count 0002") == ("0", {"count": "0"})
  assert apply({}, "INCRBY count -0") == ("0", {"count": "0"})
  assert apply({}, " \tINCR\tcount\r ") == ("1", {"count": "1"})
  assert apply({}, " \tINCRBY\tcount\t-2\r ") == ("-2", {"count": "-2"})
  assert apply({"count": "1"}, " \tKEYS\r ") == ("count", {"count": "1"})
end

test "unknown commands and incorrect word counts leave the store unchanged"
  store = {"count": "4"}
  for line in [
    "",
    " \t",
    "PING",
    "incr count",
    "incrby count 1",
    "keys",
    "SET",
    "SET count",
    "GET",
    "GET count extra",
    "DEL",
    "DEL count extra",
    "INCR",
    "INCR count 1",
    "INCRBY",
    "INCRBY count",
    "INCRBY count 1 extra",
    "KEYS count"
  ]
    assert apply(store, line) == ("ERR unknown command", store)
  end
end

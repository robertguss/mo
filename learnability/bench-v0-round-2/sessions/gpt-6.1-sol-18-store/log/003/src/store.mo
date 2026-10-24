## A key-value store that serves commands read from standard input, one on each line.
module Store

fn increment(store: Map(Text, Text), key: Text, amount: Text) : (Text, Map(Text, Text))
  case ((store.get(key) or "0").to_int, amount.to_int)
    (Some(value), Some(delta)):
      case value.checked_add(delta)
        Some(total): (total.to_text, store.set(key, total.to_text))
        None: ("ERR overflow", store)
      end
    (None, _) | (_, None): ("ERR not an integer", store)
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

test "increments create and update integer text values"
  console = Console.fake_input([
    "INCR count",
    "INCR count",
    "INCRBY count -5",
    "GET count",
    "INCRBY other 8",
    "KEYS"
  ])
  main(console)
  assert console.printed == ["1", "2", "-3", "-3", "8", "count other"]
  assert apply({"a": "-0002"}, "INCRBY a 0003") == ("1", {"a": "1"})
  assert apply({}, "INCRBY a -0") == ("0", {"a": "0"})
  assert apply({"a": "-1"}, "INCR a") == ("0", {"a": "0"})
end

test "keys keep insertion order across updates and move to the end after deletion"
  console = Console.fake_input([
    "KEYS",
    "SET z 4",
    "SET a 2",
    "SET z 8",
    "INCR z",
    "INCRBY a -3",
    "KEYS",
    "DEL z",
    "INCR z",
    "KEYS",
    "DEL a",
    "SET a again",
    "KEYS",
    "DEL z",
    "DEL a",
    "KEYS"
  ])
  main(console)
  assert console.printed == [
    "(empty)",
    "OK",
    "OK",
    "OK",
    "9",
    "-1",
    "z a",
    "1",
    "1",
    "a z",
    "1",
    "OK",
    "z a",
    "1",
    "1",
    "(empty)"
  ]
end

test "invalid stored integers leave every entry unchanged"
  for invalid in [
    "",
    "-",
    "+1",
    "1.0",
    "1e2",
    "1_0",
    "0x10",
    "１２",
    "١",
    " 1",
    "1 ",
    "1 2",
    "--1",
    "9223372036854775808",
    "-9223372036854775809"
  ]
    store = {"first": "7", "bad": invalid, "last": "9"}
    assert apply(store, "INCR bad") == ("ERR not an integer", store)
    assert apply(store, "INCRBY bad 0") == ("ERR not an integer", store)
  end
end

test "invalid amounts neither change existing values nor create missing keys"
  for invalid in [
    "-",
    "+1",
    "1.0",
    "1e2",
    "1_0",
    "0x10",
    "１２",
    "١",
    "--1",
    "9223372036854775808",
    "-9223372036854775809"
  ]
    store = {"z": "0002", "a": "3"}
    assert apply(store, "INCRBY z #{invalid}") == ("ERR not an integer", store)
    assert apply(store, "INCRBY missing #{invalid}") == ("ERR not an integer", store)
  end
end

test "signed integer limits succeed exactly and overflowing additions preserve the store"
  store = {"high": "9223372036854775807", "low": "-9223372036854775808"}
  assert apply(store, "INCR high") == ("ERR overflow", store)
  assert apply(store, "INCRBY high 1") == ("ERR overflow", store)
  assert apply(store, "INCRBY low -1") == ("ERR overflow", store)
  assert apply(store, "INCRBY high 9223372036854775807") == ("ERR overflow", store)
  assert apply(store, "INCRBY low -9223372036854775808") == ("ERR overflow", store)
  assert apply({}, "INCRBY a 9223372036854775807") == (
    "9223372036854775807",
    {"a": "9223372036854775807"}
  )
  assert apply({}, "INCRBY a -9223372036854775808") == (
    "-9223372036854775808",
    {"a": "-9223372036854775808"}
  )
  assert apply({"a": "9223372036854775806"}, "INCR a") == (
    "9223372036854775807",
    {"a": "9223372036854775807"}
  )
  assert apply({"a": "-9223372036854775807"}, "INCRBY a -1") == (
    "-9223372036854775808",
    {"a": "-9223372036854775808"}
  )
  assert apply({"a": "9223372036854775807"}, "INCRBY a -9223372036854775808") == ("-1", {"a": "-1"})
  assert apply({"a": "-9223372036854775808"}, "INCRBY a 9223372036854775807") == ("-1", {"a": "-1"})
end

test "commands require their exact word counts and accept white space separators"
  store = {"a": "2"}
  for line in [
    "",
    " ",
    "PING",
    "SET",
    "SET a",
    "GET",
    "GET a extra",
    "DEL",
    "DEL a extra",
    "INCR",
    "INCR a extra",
    "INCRBY",
    "INCRBY a",
    "INCRBY a 2 extra",
    "KEYS a",
    "keys"
  ]
    assert apply(store, line) == ("ERR unknown command", store)
  end
  assert apply(store, " \tINCRBY\ta\u{B}-3\u{C}\r") == ("-1", {"a": "-1"})
  assert apply(store, "\tKEYS \r") == ("a", store)
end

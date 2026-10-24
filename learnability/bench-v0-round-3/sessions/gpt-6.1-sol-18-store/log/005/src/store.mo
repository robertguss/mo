## A key-value store that serves commands read from standard input, one on each line.
module Store

fn increment(store: Map(Text, Text), key: Text, amount: Text) : (Text, Map(Text, Text))
  case ((store.get(key) or "0").to_int, amount.to_int)
    (Some(value), Some(delta)):
      case value.checked_add(delta)
        Some(total):
          text = total.to_text
          (text, store.set(key, text))
        None: ("ERR overflow", store)
      end
    _: ("ERR not an integer", store)
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

test "increments create keys and store their results as text"
  console = Console.fake_input([
    "INCR count",
    "INCR count",
    "INCRBY count -5",
    "GET count",
    "INCRBY other -7",
    "GET other",
    "SET count -0002",
    "INCR count",
    "INCRBY count 0001",
    "GET count"
  ])
  main(console)
  assert console.printed == ["1", "2", "-3", "-3", "-7", "-7", "OK", "-1", "0", "0"]
end

test "invalid integers leave existing and missing keys unchanged"
  for invalid in [
    "-",
    "+1",
    "1.5",
    "1e2",
    "1_0",
    "0x10",
    "１２",
    "١",
    "--1",
    "9223372036854775808",
    "-9223372036854775809"
  ]
    store = {"first": "4", "bad": invalid, "last": "9"}
    assert apply(store, "INCR bad") == ("ERR not an integer", store)
    assert apply(store, "INCRBY bad 1") == ("ERR not an integer", store)
    assert apply(store, "INCRBY first #{invalid}") == ("ERR not an integer", store)
    assert apply(store, "INCRBY missing #{invalid}") == ("ERR not an integer", store)
  end
  for invalid in ["", " 1", "1 ", "1 2"]
    store = {"bad": invalid}
    assert apply(store, "INCR bad") == ("ERR not an integer", store)
    assert apply(store, "INCRBY bad 0") == ("ERR not an integer", store)
  end
end

test "integer boundaries succeed and overflowing additions preserve stored text"
  console = Console.fake_input([
    "INCRBY high 9223372036854775806",
    "INCR high",
    "INCR high",
    "GET high",
    "INCRBY high 1",
    "GET high",
    "INCRBY low -9223372036854775808",
    "INCRBY low -1",
    "GET low",
    "INCR low",
    "INCRBY low -1",
    "INCRBY low 9223372036854775807",
    "INCRBY high -9223372036854775808",
    "SET raw 09223372036854775807",
    "INCR raw",
    "GET raw"
  ])
  main(console)
  assert console.printed == [
    "9223372036854775806",
    "9223372036854775807",
    "ERR overflow",
    "9223372036854775807",
    "ERR overflow",
    "9223372036854775807",
    "-9223372036854775808",
    "ERR overflow",
    "-9223372036854775808",
    "-9223372036854775807",
    "-9223372036854775808",
    "-1",
    "-1",
    "OK",
    "ERR overflow",
    "09223372036854775807"
  ]
end

test "keys keep insertion order across updates deletion and reinsertion"
  console = Console.fake_input([
    "KEYS",
    "SET zebra 7",
    "SET apple 2",
    "SET zebra 8",
    "INCR zebra",
    "INCRBY apple -2",
    "INCR middle",
    "INCRBY missing +1",
    "INCRBY zebra 9223372036854775807",
    "KEYS",
    "DEL apple",
    "SET apple 3",
    "KEYS",
    "DEL zebra",
    "INCR zebra",
    "KEYS",
    "DEL missing",
    "DEL middle",
    "DEL apple",
    "DEL zebra",
    "KEYS"
  ])
  main(console)
  assert console.printed == [
    "(empty)",
    "OK",
    "OK",
    "OK",
    "9",
    "0",
    "1",
    "ERR not an integer",
    "ERR overflow",
    "zebra apple middle",
    "1",
    "OK",
    "zebra middle apple",
    "1",
    "1",
    "middle apple zebra",
    "0",
    "1",
    "1",
    "1",
    "(empty)"
  ]
end

test "command arity is exact and whitespace separates words"
  store = {"a": "2"}
  for command in [
    "",
    " \t ",
    "SET",
    "SET a",
    "GET",
    "GET a extra",
    "DEL",
    "DEL a extra",
    "INCR",
    "INCR a 1",
    "INCRBY",
    "INCRBY a",
    "INCRBY a 1 extra",
    "KEYS a",
    "keys"
  ]
    assert apply(store, command) == ("ERR unknown command", store)
  end
  assert apply(store, " \tINCR\ta \r") == ("3", {"a": "3"})
  assert apply(store, "\t INCRBY \t a \t -3 \r") == ("-1", {"a": "-1"})
  assert apply(store, " \tKEYS\r") == ("a", store)
  assert apply(store, "SET\tname\tAda   Lovelace") == ("OK", {"a": "2", "name": "Ada Lovelace"})
end

## A key-value store that serves commands read from standard input, one on each line.
module Store

## Applies one command line to the store, giving the reply and the new store.
fn apply(store: Map(Text, Text), line: Text) : (Text, Map(Text, Text))
  examples
    apply({}, "SET a 1") == ("OK", {"a": "1"})
    apply({"a": "1"}, "GET a") == ("1", {"a": "1"})
    apply({}, "GET a") == ("(nil)", {})
    apply({"a": "1"}, "DEL a") == ("1", {})
    apply({}, "PING") == ("ERR unknown command", {})
    apply({"a": "41"}, "INCR a") == ("42", {"a": "42"})
    apply({}, "INCR a") == ("1", {"a": "1"})
    apply({"a": "5"}, "INCRBY a -7") == ("-2", {"a": "-2"})
    apply({"a": "x"}, "INCR a") == ("ERR not an integer", {"a": "x"})
    apply({"a": "1"}, "INCRBY a 1.5") == ("ERR not an integer", {"a": "1"})
    apply({"a": "9223372036854775807"}, "INCR a") == ("ERR overflow", {"a": "9223372036854775807"})
    apply({"a": "1", "b": "2"}, "KEYS") == ("a b", {"a": "1", "b": "2"})
    apply({}, "KEYS") == ("(empty)", {})
  end

  case line.words
    ["SET", key, ..words] if not words.empty?: ("OK", store.set(key, words.join(" ")))
    ["GET", key]: (store.get(key) or "(nil)", store)
    ["DEL", key]: if store.has?(key): ("1", store.remove(key)) else: ("0", store)
    ["INCR", key]: add(store, key, "1")
    ["INCRBY", key, amount]: add(store, key, amount)
    ["KEYS"]: (if store.empty?: "(empty)" else: store.keys.join(" "), store)
    _: ("ERR unknown command", store)
  end
end

## Adds the whole number in `amount` to the one held at `key`, which counts as 0 when missing.
fn add(store: Map(Text, Text), key: Text, amount: Text) : (Text, Map(Text, Text))
  examples
    add({"n": "-3"}, "n", "10") == ("7", {"n": "7"})
    add({}, "n", "-9223372036854775808") == ("-9223372036854775808", {"n": "-9223372036854775808"})
    add({"n": "-1"}, "n", "-9223372036854775808") == ("ERR overflow", {"n": "-1"})
    add({"n": "1"}, "n", "+1") == ("ERR not an integer", {"n": "1"})
    add({"n": "1"}, "n", "99999999999999999999") == ("ERR not an integer", {"n": "1"})
    add({"n": ""}, "n", "1") == ("ERR not an integer", {"n": ""})
  end

  case ((store.get(key) or "0").to_int, amount.to_int)
    (Some(current), Some(step)):
      case current.checked_add(step)
        Some(total): (total.to_text, store.set(key, total.to_text))
        None: ("ERR overflow", store)
      end
    _: ("ERR not an integer", store)
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

test "counters and key order"
  console = Console.fake_input([
    "INCR hits",
    "INCRBY hits 10",
    "INCRBY hits -20",
    "SET name Ada",
    "INCR name",
    "KEYS",
    "SET hits 5",
    "DEL hits",
    "INCR hits",
    "KEYS",
    "DEL name",
    "DEL hits",
    "KEYS",
    "INCR",
    "INCRBY hits",
    "INCRBY hits 1 2",
    "KEYS extra",
    "  INCRBY\tcount   3  "
  ])
  main(console)
  assert console.printed == [
    "1",
    "11",
    "-9",
    "OK",
    "ERR not an integer",
    "hits name",
    "OK",
    "1",
    "1",
    "name hits",
    "1",
    "1",
    "(empty)",
    "ERR unknown command",
    "ERR unknown command",
    "ERR unknown command",
    "ERR unknown command",
    "3"
  ]
end

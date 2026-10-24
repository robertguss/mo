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
    apply({"b": "1", "a": "2"}, "KEYS") == ("b a", {"b": "1", "a": "2"})
    apply({}, "KEYS") == ("(empty)", {})
    apply({}, "KEYS a") == ("ERR unknown command", {})
  end

  case line.words
    ["SET", key, ..words] if not words.empty?: ("OK", store.set(key, words.join(" ")))
    ["GET", key]: (store.get(key) or "(nil)", store)
    ["DEL", key]: if store.has?(key): ("1", store.remove(key)) else: ("0", store)
    ["INCR", key]: add(store, key, 1)
    ["INCRBY", key, amount]:
      case amount.to_int
        Some(n): add(store, key, n)
        None: ("ERR not an integer", store)
      end
    ["KEYS"]: (if store.empty?: "(empty)" else: store.keys.join(" "), store)
    _: ("ERR unknown command", store)
  end
end

## Adds `amount` to the whole number held at `key`, where a missing key counts as 0.
fn add(store: Map(Text, Text), key: Text, amount: Int) : (Text, Map(Text, Text))
  examples
    add({"a": "-3"}, "a", 3) == ("0", {"a": "0"})
    add({}, "a", -1) == ("-1", {"a": "-1"})
    add({"a": "-9223372036854775808"}, "a", -1) == ("ERR overflow", {"a": "-9223372036854775808"})
    add({"a": ""}, "a", 1) == ("ERR not an integer", {"a": ""})
  end

  case (store.get(key) or "0").to_int
    None: ("ERR not an integer", store)
    Some(current):
      case current.checked_add(amount)
        Some(total): (total.to_text, store.set(key, total.to_text))
        None: ("ERR overflow", store)
      end
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
    "SET a 1",
    "SET name Grace",
    "KEYS",
    "INCR name",
    "INCRBY a x",
    "INCRBY a 9223372036854775807",
    "GET a",
    "DEL hits",
    "SET hits 3",
    "KEYS",
    "INCRBY a",
    "INCR",
    "  KEYS  ",
    "DEL name",
    "DEL a",
    "DEL hits",
    "KEYS"
  ])
  main(console)
  assert console.printed == [
    "1",
    "11",
    "-9",
    "OK",
    "OK",
    "OK",
    "hits name a",
    "ERR not an integer",
    "ERR not an integer",
    "ERR overflow",
    "1",
    "1",
    "OK",
    "name a hits",
    "ERR unknown command",
    "ERR unknown command",
    "name a hits",
    "1",
    "1",
    "1",
    "(empty)"
  ]
end

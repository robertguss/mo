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
    apply({}, "INCR n") == ("1", {"n": "1"})
    apply({"n": "41"}, "INCRBY n -50") == ("-9", {"n": "-9"})
    apply({"n": "x"}, "INCR n") == ("ERR not an integer", {"n": "x"})
    apply({}, "INCRBY n 1.5") == ("ERR not an integer", {})
    apply({"n": "9223372036854775807"}, "INCR n") == ("ERR overflow", {"n": "9223372036854775807"})
    apply({"b": "1", "a": "2"}, "KEYS") == ("b a", {"b": "1", "a": "2"})
    apply({}, "KEYS") == ("(empty)", {})
    apply({}, "INCR") == ("ERR unknown command", {})
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

## Adds the whole number written in `amount` to the one held at `key`, which counts as 0 when
## missing, giving the reply and the new store. On a failure the store is unchanged.
fn add(store: Map(Text, Text), key: Text, amount: Text) : (Text, Map(Text, Text))
  examples
    add({}, "n", "-3") == ("-3", {"n": "-3"})
    add({"n": "-9223372036854775808"}, "n", "-1") == ("ERR overflow", {"n": "-9223372036854775808"})
    add({"n": "5"}, "n", "+1") == ("ERR not an integer", {"n": "5"})
  end

  current = (store.get(key) or "0").to_int
  case (current, amount.to_int)
    (Some(a), Some(b)):
      case a.checked_add(b)
        Some(sum): (sum.to_text, store.set(key, sum.to_text))
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

test "counters and keys follow the order keys were first set"
  console = Console.fake_input([
    "INCR hits",
    "INCRBY hits 10",
    "SET name Ada",
    "INCR name",
    "KEYS",
    "SET hits 3",
    "KEYS",
    "DEL hits",
    "INCRBY hits -2",
    "KEYS",
    "INCRBY hits 9223372036854775807",
    "GET hits",
    "INCRBY hits 99999999999999999999",
    "DEL hits",
    "DEL name",
    "KEYS",
    "  INCR   spaced  ",
    "INCRBY a",
    "KEYS extra"
  ])
  main(console)
  assert console.printed == [
    "1",
    "11",
    "OK",
    "ERR not an integer",
    "hits name",
    "OK",
    "hits name",
    "1",
    "-2",
    "name hits",
    "9223372036854775805",
    "9223372036854775805",
    "ERR not an integer",
    "1",
    "1",
    "(empty)",
    "1",
    "ERR unknown command",
    "ERR unknown command"
  ]
end

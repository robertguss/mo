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

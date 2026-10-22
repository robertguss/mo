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

## Adds `amount` to the whole number held at `key`, or to 0 when there is none.
fn add(store: Map(Text, Text), key: Text, amount: Text) : (Text, Map(Text, Text))
  examples
    add({}, "n", "1") == ("1", {"n": "1"})
    add({"n": "41"}, "n", "1") == ("42", {"n": "42"})
    add({"n": "x"}, "n", "1") == ("ERR not an integer", {"n": "x"})
    add({"n": "1"}, "n", "1.5") == ("ERR not an integer", {"n": "1"})
    add({"n": "9223372036854775807"}, "n", "1") == ("ERR overflow", {"n": "9223372036854775807"})
  end

  current = case store.get(key)
    Some(text): text.to_int
    None: Some(0)
  end
  case (current, amount.to_int)
    (Some(a), Some(b)):
      case a.checked_add(b)
        Some(sum): ("#{sum}", store.set(key, "#{sum}"))
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

test "counters are added to, and keys listed in the order they were set"
  console = Console.fake_input([
    "INCR hits",
    "INCRBY hits 10",
    "SET name Ada",
    "KEYS",
    "INCR name",
    "GET hits"
  ])
  main(console)
  assert console.printed == ["1", "11", "OK", "hits name", "ERR not an integer", "11"]
end

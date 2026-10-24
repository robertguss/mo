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

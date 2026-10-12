## Kv: the core of a Redis-like key-value store.
##
## A command line such as `SET greeting "hello world"` is read into a Command.
## Applying a command to a Store at a given time gives a Reply and the store
## after it. EXPIRE gives a key a number of seconds to live; once they are up
## the key reads as missing, and the next command that touches it drops it.
## INCR reports overflow instead of wrapping. `main` serves commands from
## standard input, one a line, and prints each reply as redis-cli shows it.
module Kv
expose Store, Command(..), Reply(..), Fault(..), empty_store, parse, apply, execute

## The names of the commands Kv knows.
command_names = ["SET", "GET", "DEL", "INCR", "EXPIRE"]

## The most seconds EXPIRE accepts. A Duration any longer would overflow, and
## it would end long after the year 9999, the last a Time can hold.
longest_ttl = 9_223_372_036_854

## A command, as read from a command line.
enum Command
  SetValue(key: Text, value: Text)
  GetValue(Text)
  Delete(List(Text))
  Increment(Text)
  Expire(key: Text, seconds: Int)
end

## Why a command gets an error reply. Its text form is Redis's message.
enum Fault
  NoCommand
  UnknownCommand(Text)
  WrongArity(Text)
  UnbalancedQuotes
  NotAnInteger
  Overflow
  InvalidExpireTime
end

## A reply to a command, of the kinds Redis gives. Its text form is how
## redis-cli shows it.
enum Reply
  Okay
  Bulk(Text)
  Nil
  Integer(Int)
  Rejected(Fault)
end

## A key's value, and the time it expires, if it does.
struct Entry
  value: Text
  expires: Option(Time)
end

## The keys and their entries. An entry whose time is up reads as missing,
## though it stays here until a command touches its key.
struct Store
  entries: Map(Text, Entry)
end

## A store with no keys.
fn empty_store() : Store
  Store(entries: {})
end

## Runs one command line on the store at time `now`, giving the reply and the
## store after it. A line that isn't a command gets an error reply and leaves
## the store as it was.
fn execute(store: Store, line: Text, now: Time) : (Reply, Store)
  case parse(line)
    Ok(command): apply(store, command, now)
    Error(fault): (Rejected(fault), store)
  end
end

## Reads a command line: a command's name, in any case, then its arguments.
fn parse(line: Text) : Result(Command, Fault)
  examples
    parse("set greeting \"hello world\"") == Ok(SetValue(key: "greeting", value: "hello world"))
    parse("DEL a b") == Ok(Delete(["a", "b"]))
    parse("expire a 10") == Ok(Expire(key: "a", seconds: 10))
    parse("EXPIRE a soon") == Error(NotAnInteger)
    parse("GET") == Error(WrongArity("get"))
    parse("DEL") == Error(WrongArity("del"))
    parse("PING") == Error(UnknownCommand("PING"))
    parse("  ") == Error(NoCommand)
  end

  words = try split_words(line)
  case words
    []: Error(NoCommand)
    [name, ..args]: command_from(name, args)
  end
end

## The command that a name and its arguments ask for.
fn command_from(name: Text, args: List(Text)) : Result(Command, Fault)
  upper = name.ascii_uppercase
  return Error(UnknownCommand(name)) if not command_names.contains?(upper)
  case (upper, args)
    ("SET", [key, value]): Ok(SetValue(key: key, value: value))
    ("GET", [key]): Ok(GetValue(key))
    ("DEL", [_, .._]): Ok(Delete(args))
    ("INCR", [key]): Ok(Increment(key))
    ("EXPIRE", [key, amount]):
      ttl = try amount.to_int.or_fail(NotAnInteger)
      Ok(Expire(key: key, seconds: ttl))
    _: Error(WrongArity(upper.ascii_lowercase))
  end
end

## Splits a command line into words at white space. A word that starts with a
## double quote runs to the next double quote that isn't escaped, and may hold
## white space. Inside the quotes, `\n`, `\r` and `\t` stand for a line feed, a
## carriage return and a tab, and a backslash before any other character
## stands for that character. A closing quote must end its word.
fn split_words(line: Text) : Result(List(Text), Fault)
  examples
    split_words("  SET a  1 ") == Ok(["SET", "a", "1"])
    split_words("SET a \"two words\"") == Ok(["SET", "a", "two words"])
    split_words("SET a \"say \\\"hi\\\"\"") == Ok(["SET", "a", "say \"hi\""])
    split_words("SET a \"\"") == Ok(["SET", "a", ""])
    split_words("") == Ok([])
    split_words("GET \"a") == Error(UnbalancedQuotes)
    split_words("GET \"a\"b") == Error(UnbalancedQuotes)
  end

  var words = []
  var word = []
  var in_word? = false
  var in_quotes? = false
  var escaping? = false
  var closed? = false
  for c in line.chars
    if escaping?
      word = word.append(unescaped(c))
      escaping? = false
    else if in_quotes?
      if c == '\\'
        escaping? = true
      else if c == '"'
        in_quotes? = false
        closed? = true
      else
        word = word.append(c)
      end
    else if c.ascii_white_space?
      if in_word?
        words = words.append(Text.from_chars(word))
        word = []
        in_word? = false
        closed? = false
      end
    else if closed?
      return Error(UnbalancedQuotes)
    else if c == '"' and not in_word?
      in_word? = true
      in_quotes? = true
    else
      word = word.append(c)
      in_word? = true
    end
  end
  return Error(UnbalancedQuotes) if in_quotes?
  if in_word?: Ok(words.append(Text.from_chars(word))) else: Ok(words)
end

## The character that a backslash before `c` stands for, inside double quotes.
fn unescaped(c: Char) : Char
  examples
    unescaped('n') == '\n'
    unescaped('"') == '"'
  end

  case c
    'n': '\n'
    'r': '\r'
    't': '\t'
    _: c
  end
end

## Applies a command to the store at time `now`, giving the reply and the store
## after it.
fn apply(store: Store, command: Command, now: Time) : (Reply, Store)
  case command
    SetValue(key: key, value: value): set_key(store, key, value)
    GetValue(key): get_key(store, key, now)
    Delete(keys): delete_keys(store, keys, now)
    Increment(key): increment_key(store, key, now)
    Expire(key: key, seconds: ttl): expire_key(store, key, ttl, now)
  end
end

## SET: stores the value, replacing any value and expiry time the key had.
fn set_key(store: Store, key: Text, value: Text) : (Reply, Store)
  (Okay, store.put(key, Entry(value: value, expires: None)))
end

## GET: the key's value, or nil when it has none.
fn get_key(store: Store, key: Text, now: Time) : (Reply, Store)
  case store.live(key, now)
    Some(entry): (Bulk(entry.value), store)
    None: (Nil, store.without(key))
  end
end

## DEL: removes the keys, answering how many of them had values.
fn delete_keys(store: Store, keys: List(Text), now: Time) : (Reply, Store)
  removed = keys.unique.count(fn(key) store.live(key, now) is Some(_) end)
  (Integer(removed), keys.reduce(store, fn(rest, key) rest.without(key) end))
end

## INCR: adds one to the key's value, which must be a whole number, and answers
## the sum. A key with no value counts as 0, and the key keeps its expiry time.
## A sum past the largest Int is an overflow, and changes nothing.
fn increment_key(store: Store, key: Text, now: Time) : (Reply, Store)
  entry = store.live(key, now) or Entry(value: "0", expires: None)
  n = case entry.value.to_int
    Some(found): found
    None: return (Rejected(NotAnInteger), store)
  end
  case n.checked_add(1)
    Some(next): (Integer(next), store.put(key, Entry(value: next.to_text, expires: entry.expires)))
    None: (Rejected(Overflow), store)
  end
end

## EXPIRE: gives a key `ttl` seconds to live from `now`, answering 1, or 0 when
## the key has no value. A time to live that isn't positive deletes the key.
fn expire_key(store: Store, key: Text, ttl: Int, now: Time) : (Reply, Store)
  entry = case store.live(key, now)
    Some(found): found
    None: return (Integer(0), store.without(key))
  end
  return (Integer(1), store.without(key)) if ttl <= 0
  case deadline(now, ttl)
    Some(at): (Integer(1), store.put(key, Entry(value: entry.value, expires: Some(at))))
    None: (Rejected(InvalidExpireTime), store)
  end
end

## The time `ttl` seconds after `now`, or None when that is past the times a
## Time can hold.
fn deadline(now: Time, ttl: Int) : Option(Time)
  return None if ttl > longest_ttl
  now.checked_add(ttl.seconds)
end

## The key's entry, unless it has none or its time is up at `now`.
fn live(store: Store, key: Text, now: Time) : Option(Entry)
  case store.entries.get(key)
    Some(entry) if entry.alive?(now): Some(entry)
    Some(_) | None: None
  end
end

## Whether the entry still has its value at `now`: it never expires, or its
## expiry time is still to come.
fn alive?(entry: Entry, now: Time) : Bool
  case entry.expires
    Some(at): now < at
    None: true
  end
end

## The store with the key set to the entry.
fn put(store: Store, key: Text, entry: Entry) : Store
  Store(entries: store.entries.set(key, entry))
end

## The store without the key.
fn without(store: Store, key: Text) : Store
  Store(entries: store.entries.remove(key))
end

## A reply as redis-cli shows it.
fn to_text(reply: Reply) : Text
  examples
    to_text(Okay) == "OK"
    to_text(Bulk("two words")) == "\"two words\""
    to_text(Nil) == "(nil)"
    to_text(Integer(-3)) == "(integer) -3"
    to_text(Rejected(Overflow)) == "(error) ERR increment or decrement would overflow"
  end

  case reply
    Okay: "OK"
    Bulk(value): quoted(value)
    Nil: "(nil)"
    Integer(n): "(integer) #{n}"
    Rejected(fault): "(error) #{fault}"
  end
end

## A fault in Redis's words.
fn to_text(fault: Fault) : Text
  examples
    to_text(WrongArity("get")) == "ERR wrong number of arguments for 'get' command"
  end

  case fault
    NoCommand: "ERR empty command"
    UnknownCommand(name): "ERR unknown command '#{name}'"
    WrongArity(command): "ERR wrong number of arguments for '#{command}' command"
    UnbalancedQuotes: "ERR Protocol error: unbalanced quotes in request"
    NotAnInteger: "ERR value is not an integer or out of range"
    Overflow: "ERR increment or decrement would overflow"
    InvalidExpireTime: "ERR invalid expire time in 'expire' command"
  end
end

## The value in double quotes, escaped so that it reads back as itself.
fn quoted(value: Text) : Text
  promise "the quoted value reads back as one word, the value itself"
    split_words(result) == Ok([value])
    allows value: "two words", result: "\"two words\""
    forbids value: "two words", result: "two words"
  end

  escaped = value
    .replace("\\", "\\\\")
    .replace("\"", "\\\"")
    .replace("\n", "\\n")
    .replace("\r", "\\r")
    .replace("\t", "\\t")
  "\"#{escaped}\""
end

## Serves commands from standard input, one a line, printing each reply. Each
## command runs at the time it is read, and blank lines are skipped.
fn main(console: Console, clock: Clock)
  var store = empty_store()
  while console.read_line is Some(line)
    continue if line.trim.empty?
    (reply, after) = execute(store, line, clock.now)
    console.print("#{reply}")
    store = after
  end
end

## The replies to command lines run in order on an empty store, each paired
## with how many seconds after `start` it runs.
fn replay(steps: List((Int, Text)), start: Time) : List(Reply)
  var store = empty_store()
  var replies = []
  for (offset, line) in steps
    (reply, after) = execute(store, line, start + offset.seconds)
    replies = replies.append(reply)
    store = after
  end
  replies
end

test "GET gives back what SET stored, and SET replaces it"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  steps = [
    (0, "GET greeting"),
    (0, "SET greeting hello"),
    (0, "get greeting"),
    (0, "SET greeting \"hello world\""),
    (0, "GET greeting")
  ]
  assert replay(steps, start) == [Nil, Okay, Bulk("hello"), Okay, Bulk("hello world")]
end

test "DEL removes keys and counts those that had values"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  steps = [
    (0, "SET a 1"),
    (0, "SET b 2"),
    (0, "DEL a b c a"),
    (0, "GET a"),
    (0, "DEL a")
  ]
  assert replay(steps, start) == [Okay, Okay, Integer(2), Nil, Integer(0)]
end

test "INCR counts up from nothing, and reports overflow and values that aren't numbers"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  steps = [
    (0, "INCR hits"),
    (0, "INCR hits"),
    (0, "SET hits 9223372036854775807"),
    (0, "INCR hits"),
    (0, "GET hits"),
    (0, "SET name ada"),
    (0, "INCR name")
  ]
  assert replay(steps, start) == [
    Integer(1),
    Integer(2),
    Okay,
    Rejected(Overflow),
    Bulk("9223372036854775807"),
    Okay,
    Rejected(NotAnInteger)
  ]
end

test "a key lives as many seconds as EXPIRE gives it"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  steps = [
    (0, "SET session abc"),
    (0, "EXPIRE session 10"),
    (9, "GET session"),
    (10, "GET session"),
    (10, "EXPIRE session 10"),
    (10, "DEL session")
  ]
  assert replay(steps, start) == [Okay, Integer(1), Bulk("abc"), Nil, Integer(0), Integer(0)]
end

test "INCR keeps a key's expiry time, and SET clears it"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  steps = [
    (0, "SET n 1"),
    (0, "EXPIRE n 5"),
    (1, "INCR n"),
    (5, "GET n"),
    (6, "SET n 1"),
    (6, "EXPIRE n 5"),
    (7, "SET n 2"),
    (100, "GET n")
  ]
  assert replay(steps, start) == [Okay, Integer(1), Integer(2), Nil, Okay, Integer(1), Okay, Bulk("2")]
end

test "EXPIRE deletes a key when the time to live isn't positive"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  steps = [
    (0, "SET a 1"),
    (0, "EXPIRE a 0"),
    (0, "GET a"),
    (0, "SET b 2"),
    (0, "EXPIRE b -5"),
    (0, "EXPIRE b 5")
  ]
  assert replay(steps, start) == [Okay, Integer(1), Nil, Okay, Integer(1), Integer(0)]
end

test "EXPIRE refuses a time to live that ends past the year 9999"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  steps = [
    (0, "SET a 1"),
    (0, "EXPIRE a 400000000000"),
    (0, "EXPIRE a 9223372036854775807"),
    (0, "EXPIRE a soon"),
    (0, "GET a")
  ]
  assert replay(steps, start) == [
    Okay,
    Rejected(InvalidExpireTime),
    Rejected(InvalidExpireTime),
    Rejected(NotAnInteger),
    Bulk("1")
  ]
end

test "lines that aren't commands get errors and leave the store alone"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  steps = [
    (0, "SET a 1"),
    (0, "PING"),
    (0, "GET"),
    (0, "SET a \"2"),
    (0, "GET a")
  ]
  assert replay(steps, start) == [
    Okay,
    Rejected(UnknownCommand("PING")),
    Rejected(WrongArity("get")),
    Rejected(UnbalancedQuotes),
    Bulk("1")
  ]
end

test "main answers each command on standard input and skips blank lines"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  console = Console.fake_input([
    "SET greeting \"hello world\"",
    "",
    "GET greeting",
    "INCR greeting",
    "GET nothing"
  ])
  main(console, Clock.fixed(start))
  assert console.printed == [
    "OK",
    "\"hello world\"",
    "(error) ERR value is not an integer or out of range",
    "(nil)"
  ]
end

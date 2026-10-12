## kv: the core of a Redis-like key-value store.
##
## A command line names one of SET, GET, DEL, INCR and EXPIRE, in any case,
## followed by its arguments. Applying a command to a store at a given time
## gives a reply and the store after it. main serves commands from standard
## input, one per line, and prints each reply the way redis-cli shows it.
module Kv
expose Store, Command(..), Reply(..), Problem(..), new_store, parse, apply, execute

## A command, as read from a command line.
enum Command
  Put(key: Text, value: Text)        # SET key value
  Fetch(key: Text)                   # GET key
  Delete(keys: List(Text))           # DEL key [key ...]
  Increment(key: Text)               # INCR key
  Expire(key: Text, seconds: Int)    # EXPIRE key seconds
end

## What a command answers.
enum Reply
  Done               # OK
  Value(Text)        # the value a key holds
  Nil                # no value
  Integer(Int)       # a count, or the number a key now holds
  Failure(Problem)   # the command was refused
end

## Why a command is refused.
enum Problem
  EmptyCommand
  UnbalancedQuotes
  UnknownCommand(name: Text)
  WrongArity(command: Text)
  SyntaxError
  NotAnInteger
  Overflow
  InvalidExpireTime
end

## Where split_args is within a command line.
enum Reading
  Between    # between arguments
  Bare       # in an argument, outside quotes
  Quoted     # inside double quotes
  Escaped    # just after a backslash inside double quotes
  Closed     # just after a closing quote, which must end its argument
end

## What a key holds: its value, and the time it expires, if it does.
struct Entry
  value: Text
  expires: Option(Time)
end

## The keys and what they hold. An entry that has expired may stay until a
## command next names its key, but no command ever sees it.
struct Store
  entries: Map(Text, Entry)
end

## A store with no keys.
fn new_store() : Store
  Store(entries: {})
end

# Parsing ---------------------------------------------------------------------

## Reads a command line: a command name, in any case, then its arguments.
fn parse(line: Text) : Result(Command, Problem)
  examples
    parse("set greeting hello") == Ok(Put(key: "greeting", value: "hello"))
    parse("SET greeting \"hello world\"") == Ok(Put(key: "greeting", value: "hello world"))
    parse("GET greeting") == Ok(Fetch(key: "greeting"))
    parse("DEL a b") == Ok(Delete(keys: ["a", "b"]))
    parse("incr visits") == Ok(Increment(key: "visits"))
    parse("EXPIRE session 60") == Ok(Expire(key: "session", seconds: 60))
    parse("EXPIRE session soon") == Error(NotAnInteger)
    parse("SET greeting") == Error(WrongArity(command: "set"))
    parse("SET greeting hello there") == Error(SyntaxError)
    parse("GET \"greeting") == Error(UnbalancedQuotes)
    parse("PING") == Error(UnknownCommand(name: "PING"))
    parse("   ") == Error(EmptyCommand)
  end

  words = try split_args(line)
  case words
    []: Error(EmptyCommand)
    [name, ..args]: read_command(name, args)
  end
end

## Splits a command line into its arguments, as Redis splits an inline
## command: at ASCII white space, except inside double quotes. A quoted part
## may hold white space and the escapes \n, \r and \t; a backslash before any
## other character, such as \" or \\, stands for that character. A closing
## quote must end its argument.
fn split_args(line: Text) : Result(List(Text), Problem)
  examples
    split_args("SET  greeting hello ") == Ok(["SET", "greeting", "hello"])
    split_args("SET greeting \"hello world\"") == Ok(["SET", "greeting", "hello world"])
    split_args("SET k \"say \\\"hi\\\"\\n\"") == Ok(["SET", "k", "say \"hi\"\n"])
    split_args("SET k \"\"") == Ok(["SET", "k", ""])
    split_args("") == Ok([])
    split_args("GET \"k") == Error(UnbalancedQuotes)
    split_args("GET \"k\"x") == Error(UnbalancedQuotes)
  end

  var args = []
  var word = []
  var state = Between
  for c in line.chars
    case state
      Between:
        if c == '"'
          word = []
          state = Quoted
        else if not c.ascii_white_space?
          word = [c]
          state = Bare
        end
      Bare:
        if c.ascii_white_space?
          args = args.append(Text.from_chars(word))
          state = Between
        else if c == '"'
          state = Quoted
        else
          word = word.append(c)
        end
      Quoted:
        if c == '\\'
          state = Escaped
        else if c == '"'
          state = Closed
        else
          word = word.append(c)
        end
      Escaped:
        word = word.append(unescape(c))
        state = Quoted
      Closed:
        return Error(UnbalancedQuotes) if not c.ascii_white_space?
        args = args.append(Text.from_chars(word))
        state = Between
    end
  end
  case state
    Between: Ok(args)
    Bare | Closed: Ok(args.append(Text.from_chars(word)))
    Quoted | Escaped: Error(UnbalancedQuotes)
  end
end

## The character a backslash and `c` stand for inside double quotes.
fn unescape(c: Char) : Char
  case c
    'n': '\n'
    'r': '\r'
    't': '\t'
    _: c
  end
end

## The command `name` names, with its arguments checked.
fn read_command(name: Text, args: List(Text)) : Result(Command, Problem)
  case (name.ascii_uppercase, args)
    ("SET", [key, value]): Ok(Put(key: key, value: value))
    ("GET", [key]): Ok(Fetch(key: key))
    ("DEL", [_, .._]): Ok(Delete(keys: args))
    ("INCR", [key]): Ok(Increment(key: key))
    ("EXPIRE", [key, seconds]):
      ttl = try integer(seconds)
      Ok(Expire(key: key, seconds: ttl))
    ("SET", [_, _, _, .._]) | ("EXPIRE", [_, _, _, .._]): Error(SyntaxError)
    ("SET", _) | ("GET", _) | ("DEL", _) | ("INCR", _) | ("EXPIRE", _):
      Error(WrongArity(command: name.ascii_lowercase))
    _: Error(UnknownCommand(name: name))
  end
end

## The integer `text` spells, written as Redis writes integers: digits with
## no leading zeros, after an optional `-`.
fn integer(text: Text) : Result(Int, Problem)
  promise "an integer is read only from its own spelling, so it is written back unchanged"
    result is Ok(n) implies "#{n}" == text
    allows text: "-12", result: Ok(-12)
    forbids text: "007", result: Ok(7)
  end
  examples
    integer("42") == Ok(42)
    integer("-7") == Ok(-7)
    integer("0") == Ok(0)
    integer("007") == Error(NotAnInteger)
    integer("+7") == Error(NotAnInteger)
    integer("4.5") == Error(NotAnInteger)
    integer("99999999999999999999") == Error(NotAnInteger)
  end

  case text.to_int
    Some(n) if "#{n}" == text: Ok(n)
    Some(_) | None: Error(NotAnInteger)
  end
end

# Applying commands -----------------------------------------------------------

## Applies a command to the store at time `now`: the reply, and the store
## after it.
fn apply(store: Store, command: Command, now: Time) : (Reply, Store)
  case command
    Put(key: key, value: value): (Done, store.put(key, Entry(value: value, expires: None)))
    Fetch(key: key): fetch(store.sweep(key, now), key)
    Delete(keys: keys): delete(store, keys, now)
    Increment(key: key): increment(store.sweep(key, now), key)
    Expire(key: key, seconds: ttl): expire(store.sweep(key, now), key, ttl, now)
  end
end

## Reads and applies one command line at time `now`. A line that can't be
## read is refused, and leaves the store as it was.
fn execute(store: Store, line: Text, now: Time) : (Reply, Store)
  case parse(line)
    Ok(command): apply(store, command, now)
    Error(problem): (Failure(problem), store)
  end
end

## The value at `key`.
fn fetch(store: Store, key: Text) : (Reply, Store)
  case store.entries.get(key)
    Some(entry): (Value(entry.value), store)
    None: (Nil, store)
  end
end

## Removes the keys, counting those that held a value at `now`.
fn delete(store: Store, keys: List(Text), now: Time) : (Reply, Store)
  var remaining = store.entries
  var removed = 0
  for key in keys
    if remaining.get(key) is Some(entry)
      if entry.live?(now)
        removed += 1
      end
      remaining = remaining.remove(key)
    end
  end
  (Integer(removed), Store(entries: remaining))
end

## Adds one to the integer at `key`, counting a missing key as 0. The key
## keeps its expiry.
fn increment(store: Store, key: Text) : (Reply, Store)
  old = store.entries.get(key) or Entry(value: "0", expires: None)
  case plus_one(old.value)
    Ok(n): (Integer(n), store.put(key, Entry(value: "#{n}", expires: old.expires)))
    Error(problem): (Failure(problem), store)
  end
end

## The integer `text` spells, plus one.
fn plus_one(text: Text) : Result(Int, Problem)
  promise "a success is one more than the integer the text spells"
    result is Ok(n) implies integer(text) == Ok(n - 1)
    allows text: "41", result: Ok(42)
    forbids text: "41", result: Ok(41)
  end
  examples
    plus_one("41") == Ok(42)
    plus_one("-1") == Ok(0)
    plus_one("9223372036854775807") == Error(Overflow)
    plus_one("forty") == Error(NotAnInteger)
  end

  n = try integer(text)
  n.checked_add(1).or_fail(Overflow)
end

## Sets `key` to expire `ttl` seconds after `now`, answering 1, or 0 when the
## key holds nothing. A time to live that isn't positive removes the key at
## once.
fn expire(store: Store, key: Text, ttl: Int, now: Time) : (Reply, Store)
  at = case deadline(now, ttl)
    Some(time): time
    None: return (Failure(InvalidExpireTime), store)
  end
  case store.entries.get(key)
    Some(entry):
      expiring = Entry(value: entry.value, expires: Some(at))
      (Integer(1), store.put(key, expiring).sweep(key, now))
    None: (Integer(0), store)
  end
end

## The time `ttl` seconds after `now`, or `now` itself when `ttl` isn't
## positive; None when that is past the year 9999.
fn deadline(now: Time, ttl: Int) : Option(Time)
  return Some(now) if ttl <= 0
  return None if ttl > longest_ttl
  now.checked_add(ttl.seconds)
end

## More seconds than lie between the years 1 and 9999, yet few enough that
## they fit in a Duration.
longest_ttl = 1_000_000_000_000

# The store -------------------------------------------------------------------

## The store with `key` holding `entry`.
fn put(store: Store, key: Text, entry: Entry) : Store
  Store(entries: store.entries.set(key, entry))
end

## The store without `key`, if what it holds has expired by `now`.
fn sweep(store: Store, key: Text, now: Time) : Store
  case store.entries.get(key)
    Some(entry) if not entry.live?(now): Store(entries: store.entries.remove(key))
    Some(_) | None: store
  end
end

## Whether an entry still holds at `now`. An entry that expires at a time is
## gone from that time on.
fn live?(entry: Entry, now: Time) : Bool
  case entry.expires
    Some(at): now < at
    None: true
  end
end

# Replies ---------------------------------------------------------------------

## A reply as redis-cli shows it.
fn to_text(reply: Reply) : Text
  examples
    to_text(Done) == "OK"
    to_text(Value("hello world")) == "\"hello world\""
    to_text(Nil) == "(nil)"
    to_text(Integer(-3)) == "(integer) -3"
    to_text(Failure(Overflow)) == "(error) ERR increment or decrement would overflow"
  end

  case reply
    Done: "OK"
    Value(text): quote(text)
    Nil: "(nil)"
    Integer(n): "(integer) #{n}"
    Failure(problem): "(error) #{problem}"
  end
end

## A problem as Redis reports it.
fn to_text(problem: Problem) : Text
  case problem
    EmptyCommand: "ERR empty command"
    UnbalancedQuotes: "ERR unbalanced quotes in request"
    UnknownCommand(name: name): "ERR unknown command '#{name}'"
    WrongArity(command: command): "ERR wrong number of arguments for '#{command}' command"
    SyntaxError: "ERR syntax error"
    NotAnInteger: "ERR value is not an integer or out of range"
    Overflow: "ERR increment or decrement would overflow"
    InvalidExpireTime: "ERR invalid expire time in 'expire' command"
  end
end

## `text` in double quotes, escaped so that it reads back as one argument.
fn quote(text: Text) : Text
  promise "the quoted text reads back as the text itself, as one argument"
    split_args(result) == Ok([text])
    allows text: "say \"hi\"", result: "\"say \\\"hi\\\"\""
    forbids text: "say hi", result: "say hi"
  end
  examples
    quote("hello") == "\"hello\""
    quote("") == "\"\""
    quote("a\\b \"c\"\n") == "\"a\\\\b \\\"c\\\"\\n\""
  end

  escaped = text.chars.map(escape).join("")
  "\"#{escaped}\""
end

## How `c` is written inside double quotes.
fn escape(c: Char) : Text
  case c
    '\\': "\\\\"
    '"': "\\\""
    '\n': "\\n"
    '\r': "\\r"
    '\t': "\\t"
    _: c.to_text
  end
end

# Serving ---------------------------------------------------------------------

## Serves commands from standard input, one per line, at the time each line
## arrives, and prints a reply to each. Blank lines are skipped.
fn main(console: Console, clock: Clock)
  var store = new_store()
  while console.read_line is Some(line)
    continue if line.trim.empty?
    (reply, updated) = execute(store, line, clock.now)
    console.print("#{reply}")
    store = updated
  end
end

# Tests -----------------------------------------------------------------------

## The replies, as main prints them, to command lines run in order from an
## empty store. Each step gives the seconds after `start` at which its line
## runs.
fn transcript(start: Time, steps: List((Int, Text))) : List(Text)
  var store = new_store()
  var shown = []
  for (offset, line) in steps
    (reply, updated) = execute(store, line, start + offset.seconds)
    shown = shown.append("#{reply}")
    store = updated
  end
  shown
end

test "SET and GET read a value back, and a missing key reads as nil"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  assert transcript(start, [
    (0, "SET greeting \"hello world\""),
    (0, "get greeting"),
    (0, "GET nobody"),
    (0, "SET greeting again"),
    (0, "GET greeting"),
    (0, "SET quote \"say \\\"hi\\\"\""),
    (0, "GET quote")
  ]) == ["OK", "\"hello world\"", "(nil)", "OK", "\"again\"", "OK", "\"say \\\"hi\\\"\""]
end

test "INCR counts from zero, and refuses overflow and values that aren't integers"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  assert transcript(start, [
    (0, "INCR visits"),
    (0, "INCR visits"),
    (0, "GET visits"),
    (0, "SET big 9223372036854775807"),
    (0, "INCR big"),
    (0, "GET big"),
    (0, "SET name ada"),
    (0, "INCR name"),
    (0, "SET padded 007"),
    (0, "INCR padded")
  ]) == [
    "(integer) 1",
    "(integer) 2",
    "\"2\"",
    "OK",
    "(error) ERR increment or decrement would overflow",
    "\"9223372036854775807\"",
    "OK",
    "(error) ERR value is not an integer or out of range",
    "OK",
    "(error) ERR value is not an integer or out of range"
  ]
end

test "a key expires once its time to live has passed"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  assert transcript(start, [
    (0, "SET session abc"),
    (0, "EXPIRE session 10"),
    (9, "GET session"),
    (10, "GET session"),
    (10, "EXPIRE session 10"),
    (10, "INCR session")
  ]) == ["OK", "(integer) 1", "\"abc\"", "(nil)", "(integer) 0", "(integer) 1"]
end

test "SET clears an expiry, and INCR keeps it"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  assert transcript(start, [
    (0, "SET a 1"),
    (0, "EXPIRE a 5"),
    (0, "SET a 2"),
    (0, "SET b 1"),
    (0, "EXPIRE b 5"),
    (1, "INCR b"),
    (6, "GET a"),
    (6, "GET b")
  ]) == ["OK", "(integer) 1", "OK", "OK", "(integer) 1", "(integer) 2", "\"2\"", "(nil)"]
end

test "DEL counts the keys it removed, and not those that had expired"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  assert transcript(start, [
    (0, "SET a 1"),
    (0, "SET b 2"),
    (0, "SET c 3"),
    (0, "EXPIRE c 5"),
    (6, "DEL a b c d a"),
    (6, "GET a"),
    (6, "DEL")
  ]) == [
    "OK",
    "OK",
    "OK",
    "(integer) 1",
    "(integer) 2",
    "(nil)",
    "(error) ERR wrong number of arguments for 'del' command"
  ]
end

test "EXPIRE removes a key at once when the time isn't positive, and refuses times past 9999"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  assert transcript(start, [
    (0, "SET a 1"),
    (0, "EXPIRE a 0"),
    (0, "GET a"),
    (0, "SET b 1"),
    (0, "EXPIRE b -10"),
    (0, "GET b"),
    (0, "SET c 1"),
    (0, "EXPIRE c 999999999999999"),
    (0, "EXPIRE c 300000000000"),
    (0, "EXPIRE c soon"),
    (0, "GET c")
  ]) == [
    "OK",
    "(integer) 1",
    "(nil)",
    "OK",
    "(integer) 1",
    "(nil)",
    "OK",
    "(error) ERR invalid expire time in 'expire' command",
    "(error) ERR invalid expire time in 'expire' command",
    "(error) ERR value is not an integer or out of range",
    "\"1\""
  ]
end

test "a refused command leaves the store as it was"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  (_, store) = execute(new_store(), "SET k v", start)
  assert execute(store, "SET k", start) == (Failure(WrongArity(command: "set")), store)
  assert execute(store, "INCR k", start) == (Failure(NotAnInteger), store)
  assert execute(store, "GET \"k", start) == (Failure(UnbalancedQuotes), store)
end

test "main answers each command line and skips blank ones"
  assert Time.parse("2026-10-12T09:00:00Z") is Some(start)
  console = Console.fake_input(["set counter 41", "incr counter", "", "get counter", "ping"])
  main(console, Clock.fixed(start))
  assert console.printed == [
    "OK",
    "(integer) 42",
    "\"42\"",
    "(error) ERR unknown command 'ping'"
  ]
end

## An immutable Redis-like store with a line-oriented console interface.
## Arguments are ASCII-whitespace-separated words; quoting is not supported.
module Kv
expose Store, Command(..), CommandError(..), Reply(..), empty, parse, apply, run_line

struct Entry
  value: Text
  expires_at: Option(Time)
end

struct Store
  entries: Map(Text, Entry)
end

enum Command
  Put(key: Text, value: Text)
  Fetch(Text)
  Delete(List(Text))
  Increment(Text)
  Expire(key: Text, seconds: Int)
end

enum CommandError
  EmptyCommand
  UnknownCommand(Text)
  WrongArity(Text)
  InvalidSeconds(Text)
end

enum Reply
  Status(Text)
  Value(Option(Text))
  Integer(Int)
  Failure(Text)
end

fn empty() : Store
  Store(entries: {})
end

## Parse one command, validating its name, arity and numeric arguments.
fn parse(line: Text) : Result(Command, CommandError)
  case line.words
    []: Error(EmptyCommand)
    [verb, ..args]:
      name = verb.ascii_uppercase
      case name
        "SET":
          case args
            [key, value]: Ok(Put(key: key, value: value))
            _: Error(WrongArity(name))
          end
        "GET":
          case args
            [key]: Ok(Fetch(key))
            _: Error(WrongArity(name))
          end
        "DEL":
          if args.empty?: Error(WrongArity(name)) else: Ok(Delete(args))
        "INCR":
          case args
            [key]: Ok(Increment(key))
            _: Error(WrongArity(name))
          end
        "EXPIRE":
          case args
            [key, seconds_text]:
              seconds = try seconds_text.to_int.or_fail(InvalidSeconds(seconds_text))
              Ok(Expire(key: key, seconds: seconds))
            _: Error(WrongArity(name))
          end
        _: Error(UnknownCommand(verb))
      end
  end
end

## Expiry is inclusive: a key is absent at its exact deadline.
fn purge(store: Store, now: Time) : Store
  var live = store
  for (key, entry) in store.entries
    if entry.expires_at is Some(deadline) and deadline <= now
      live.entries = live.entries.remove(key)
    end
  end
  live
end

## Overflow and non-integer values leave the live entry unchanged.
fn increment(store: Store, key: Text) : (Reply, Store)
  entry = store.entries.get(key) or Entry(value: "0", expires_at: None)
  case entry.value.to_int
    None: (Failure("ERR value is not an integer or out of range"), store)
    Some(number):
      case number.checked_add(1)
        None: (Failure("ERR increment would overflow"), store)
        Some(next):
          updated = Entry(value: next.to_text, expires_at: entry.expires_at)
          (Integer(next), Store(entries: store.entries.set(key, updated)))
      end
  end
end

## Check Duration construction as well as Time addition before changing a TTL.
fn expire(store: Store, key: Text, seconds: Int, now: Time) : (Reply, Store)
  case store.entries.get(key)
    None: (Integer(0), store)
    Some(entry):
      if seconds <= 0
        return (Integer(1), Store(entries: store.entries.remove(key)))
      end
      case seconds.checked_mul(1_000_000)
        None: (Failure("ERR expiration is out of range"), store)
        Some(_):
          case now.checked_add(seconds.seconds)
            None: (Failure("ERR expiration is out of range"), store)
            Some(deadline):
              updated = Entry(value: entry.value, expires_at: Some(deadline))
              (Integer(1), Store(entries: store.entries.set(key, updated)))
          end
      end
  end
end

## Apply a parsed command at one supplied instant, returning (reply, new_store).
fn apply(store: Store, command: Command, now: Time) : (Reply, Store)
  live = purge(store, now)
  case command
    Put(key: key, value: value):
      entry = Entry(value: value, expires_at: None)
      (Status("OK"), Store(entries: live.entries.set(key, entry)))
    Fetch(key):
      found = live.entries.get(key).map(fn(entry) entry.value end)
      (Value(found), live)
    Delete(keys):
      var remaining = live
      var removed = 0
      for key in keys
        if remaining.entries.has?(key)
          remaining.entries = remaining.entries.remove(key)
          removed += 1
        end
      end
      (Integer(removed), remaining)
    Increment(key): increment(live, key)
    Expire(key: key, seconds: seconds): expire(live, key, seconds, now)
  end
end

fn command_error_text(problem: CommandError) : Text
  case problem
    EmptyCommand: "ERR empty command"
    UnknownCommand(name): "ERR unknown command '#{name}'"
    WrongArity(name): "ERR wrong number of arguments for '#{name.ascii_lowercase}' command"
    InvalidSeconds(value): "ERR expiration is not a signed 64-bit integer: #{value}"
  end
end

## Invalid command lines return an error reply and leave the input store intact.
fn run_line(store: Store, line: Text, now: Time) : (Reply, Store)
  case parse(line)
    Ok(command): apply(store, command, now)
    Error(problem): (Failure(command_error_text(problem)), store)
  end
end

## Human-readable replies, one per input line; this is not the RESP wire protocol.
fn to_text(reply: Reply) : Text
  case reply
    Status(text): text
    Value(Some(text)): text
    Value(None): "(nil)"
    Integer(number): number.to_text
    Failure(text): text
  end
end

fn main(console: Console, clock: Clock)
  var store = empty()
  while console.read_line is Some(line)
    (reply, next_store) = run_line(store, line, clock.now)
    store = next_store
    console.print(reply.to_text)
  end
end

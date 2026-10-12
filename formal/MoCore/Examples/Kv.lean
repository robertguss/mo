/-
  examples/kv.mo in Core.

  Exercises: parsing with patterns on a tuple of a text and a list, maps,
  numbers at their limits (`checked_add`), times and durations, iterating a
  map, a `while` loop over standard input with a `case` that assigns a `var`,
  and a constant that may `stop`.
-/
import MoCore.Testing

namespace MoCore.Examples.Kv

open MoCore DSL Lib

/-- `struct Entry  value: Text  expires: Option(Time)  end` -/
def Entry : DataDecl := record "Entry" [("value", tText), ("expires", tOpt tTime)]

/-- `struct Store  entries: Map(Text, Entry)  end` -/
def Store : DataDecl := record "Store" [("entries", tMap tText Entry.ty)]

/-- ```
enum Command
  Put(key: Text, value: Text)
  Get(key: Text)
  Del(keys: List(Text))
  Incr(key: Text)
  Expire(key: Text, seconds: Int)
end
``` -/
def Command : DataDecl :=
  choice "Command" [("Put", [("key", tText), ("value", tText)]), ("Get", [("key", tText)]),
    ("Del", [("keys", tList tText)]), ("Incr", [("key", tText)]),
    ("Expire", [("key", tText), ("seconds", tInt)])]

/-- `enum Reply  Okay  Value(Text)  NoValue  Count(Int)  Failure(message: Text)  end` -/
def Reply : DataDecl :=
  choice "Reply" [("Okay", []), ("Value", [("0", tText)]), ("NoValue", []), ("Count", [("0", tInt)]),
    ("Failure", [("message", tText)])]

/-- ```
enum ParseError
  Empty
  UnknownCommand(name: Text)
  WrongArguments(command: Text)
  NotAnInteger(text: Text)
end
``` -/
def ParseError : DataDecl :=
  choice "ParseError" [("Empty", []), ("UnknownCommand", [("name", tText)]),
    ("WrongArguments", [("command", tText)]), ("NotAnInteger", [("text", tText)])]

def cmdRes : Ty := tRes Command.ty ParseError.ty
def stepTy : Ty := tTup [Store.ty, Reply.ty]
def entries (e : Expr) : Expr := get Store "entries" e
def emop (op : MapOp) (args : List Expr) : Expr := mop tText Entry.ty op args
def okC (e : Expr) : Expr := ok_ Command.ty ParseError.ty e
def failure (m : String) : Expr := mkC Reply "Failure" [str m]

/-- `max_expiry = 3_153_600_000` -/
def maxExpiry : ConstDecl := const "max_expiry" tInt (int 3153600000)

/-- `empty_store = Store(entries: {})` -/
def emptyStore : ConstDecl := const "empty_store" Store.ty (mk Store [emptyMap tText Entry.ty])

/-- `t0 = Time.parse("2026-10-12T09:00:00Z") or stop "the time is written correctly"` -/
def t0 : ConstDecl :=
  const "t0" tTime <|
    orElse (prim (.time .parse) [str "2026-10-12T09:00:00Z"]) (stop "the time is written correctly")

/-- ```
fn to_text(reply: Reply) : Text
  case reply
    Okay: "OK"
    Value(text): "\"#{text}\""
    NoValue: "(nil)"
    Count(n): "(integer) #{n}"
    Failure(message: m): "(error) #{m}"
  end
end
``` -/
def replyText : FunDecl :=
  fn "Reply.to_text" [("reply", Reply.ty)] tText <|
    .case_ (v "reply")
      [("Okay", [], str "OK"),
       ("Value", ["text"], cat [str "\"", v "text", str "\""]),
       ("NoValue", [], str "(nil)"),
       ("Count", ["n"], cat [str "(integer) ", intText (v "n")]),
       ("Failure", ["m"], cat [str "(error) ", v "m"])]

/-- ```
fn parse(line: Text) : Result(Command, ParseError)
  case line.words
    []: Error(Empty)
    [name, ..args]: build_command(name.ascii_uppercase, args)
  end
end
``` -/
def parse : FunDecl :=
  fn "parse" [("line", tText)] cmdRes <|
    caseOpt (lop tText .uncons [top .words [v "line"]]) "%p"
      (call "build_command" [top .upper [fst (v "%p")], snd (v "%p")])
      (err_ Command.ty ParseError.ty (mkC ParseError "Empty" []))

/-- ```
fn build_command(name: Text, args: List(Text)) : Result(Command, ParseError)
  case (name, args)
    ("SET", [key, value]): Ok(Put(key: key, value: value))
    ("GET", [key]): Ok(Get(key: key))
    ("DEL", [_, .._]): Ok(Del(keys: args))
    ("INCR", [key]): Ok(Incr(key: key))
    ("EXPIRE", [key, seconds]):
      n = try seconds.to_int.or_fail(NotAnInteger(text: seconds))
      Ok(Expire(key: key, seconds: n))
    _ if ["SET", "GET", "DEL", "INCR", "EXPIRE"].contains?(name): Error(WrongArguments(command: name))
    _: Error(UnknownCommand(name: name))
  end
end
```
A decision tree on the tuple: the text first, then the list's shape; every
failure goes to the last two arms, the join point `%other`. -/
def buildCommand : FunDecl :=
  fn "build_command" [("name", tText), ("args", tList tText)] cmdRes <|
    .join "%other" []
      (ite (lop tText .contains [strs ["SET", "GET", "DEL", "INCR", "EXPIRE"], v "name"])
        (err_ Command.ty ParseError.ty (mkC ParseError "WrongArguments" [v "name"]))
        (err_ Command.ty ParseError.ty (mkC ParseError "UnknownCommand" [v "name"]))) <|
      ite (teq (v "name") (str "SET"))
        (matchList tText (v "args") ["key", "value"] none
          (okC (mkC Command "Put" [v "key", v "value"])) (.jump "%other" [])) <|
      ite (teq (v "name") (str "GET"))
        (matchList tText (v "args") ["key"] none (okC (mkC Command "Get" [v "key"])) (.jump "%other" [])) <|
      ite (teq (v "name") (str "DEL"))
        (matchList tText (v "args") ["_"] (some "_") (okC (mkC Command "Del" [v "args"])) (.jump "%other" [])) <|
      ite (teq (v "name") (str "INCR"))
        (matchList tText (v "args") ["key"] none (okC (mkC Command "Incr" [v "key"])) (.jump "%other" [])) <|
      ite (teq (v "name") (str "EXPIRE"))
        (matchList tText (v "args") ["key", "seconds"] none
          (tryRes Command.ty ParseError.ty
            (orFail tInt ParseError.ty (top .toInt [v "seconds"]) (mkC ParseError "NotAnInteger" [v "seconds"]))
            "n" <|
            okC (mkC Command "Expire" [v "key", v "n"]))
          (.jump "%other" []))
        (.jump "%other" [])

/-- ```
fn apply(store: Store, command: Command, now: Time) : (Store, Reply)
  live = without_expired(store, now)
  case command
    Put(key: key, value: value): (put(live, key, Entry(value: value, expires: None)), Okay)
    Get(key: key):
      case live.entries.get(key)
        Some(entry): (live, Value(entry.value))
        None: (live, NoValue)
      end
    Del(keys: keys):
      var after = live
      for key in keys
        after.entries = after.entries.remove(key)
      end
      (after, Count(live.entries.size - after.entries.size))
    Incr(key: key): increment(live, key)
    Expire(key: key, seconds: n): expire(live, key, n, now)
  end
end
``` -/
def apply : FunDecl :=
  fn "apply" [("store", Store.ty), ("command", Command.ty), ("now", tTime)] stepTy <|
    lets [("live", call "without_expired" [v "store", v "now"])] <|
      .case_ (v "command")
        [("Put", ["key", "value"],
           tup [call "put" [v "live", v "key", mk Entry [v "value", none_ tTime]], mkC Reply "Okay" []]),
         ("Get", ["key"],
           caseOpt (emop .get [entries (v "live"), v "key"]) "entry"
             (tup [v "live", mkC Reply "Value" [get Entry "value" (v "entry")]])
             (tup [v "live", mkC Reply "NoValue" []])),
         ("Del", ["keys"],
           lets [("after", v "live")] <|
             forList "del" tText "key" (v "keys") [("after", Store.ty)] [v "after"]
               (nextRound "del" [setField Store "entries" (v "after") (emop .remove [entries (v "after"), v "key"])])
               (tup [v "after", mkC Reply "Count"
                 [isub (emop .size [entries (v "live")]) (emop .size [entries (v "after")])]])),
         ("Incr", ["key"], call "increment" [v "live", v "key"]),
         ("Expire", ["key", "n"], call "expire" [v "live", v "key", v "n", v "now"])]

/-- ```
fn increment(store: Store, key: Text) : (Store, Reply)
  entry = store.entries.get(key) or Entry(value: "0", expires: None)
  case entry.value.to_int
    None: (store, Failure(message: "the value is not an integer"))
    Some(n):
      case n.checked_add(1)
        None: (store, Failure(message: "the increment would overflow"))
        Some(more):
          var updated = entry
          updated.value = "#{more}"
          (put(store, key, updated), Count(more))
      end
  end
end
``` -/
def increment : FunDecl :=
  fn "increment" [("store", Store.ty), ("key", tText)] stepTy <|
    lets [("entry", orElse (emop .get [entries (v "store"), v "key"]) (mk Entry [str "0", none_ tTime]))] <|
      caseOpt (top .toInt [get Entry "value" (v "entry")]) "n"
        (caseOpt (iop .checkedAdd [v "n", int 1]) "more"
          (lets [("updated", v "entry"),
                 ("updated", setField Entry "value" (v "updated") (intText (v "more")))] <|
            tup [call "put" [v "store", v "key", v "updated"], mkC Reply "Count" [v "more"]])
          (tup [v "store", failure "the increment would overflow"]))
        (tup [v "store", failure "the value is not an integer"])

/-- ```
fn expire(store: Store, key: Text, ttl: Int, now: Time) : (Store, Reply)
  return (store, Failure(message: "invalid expire time")) if ttl > max_expiry
  case store.entries.get(key)
    None: (store, Count(0))
    Some(_) if ttl <= 0: (Store(entries: store.entries.remove(key)), Count(1))
    Some(entry):
      case now.checked_add(ttl.seconds)
        None: (store, Failure(message: "invalid expire time"))
        Some(at):
          var timed = entry
          timed.expires = Some(at)
          (put(store, key, timed), Count(1))
      end
  end
end
``` -/
def expire : FunDecl :=
  fn "expire" [("store", Store.ty), ("key", tText), ("ttl", tInt), ("now", tTime)] stepTy <|
    ite (igt (v "ttl") (global "max_expiry")) (tup [v "store", failure "invalid expire time"]) <|
      caseOpt (emop .get [entries (v "store"), v "key"]) "entry"
        (ite (ile (v "ttl") (int 0))
          (tup [mk Store [emop .remove [entries (v "store"), v "key"]], mkC Reply "Count" [int 1]])
          (caseOpt (prim (.time .checkedAdd) [v "now", iop .seconds [v "ttl"]]) "at"
            (lets [("timed", v "entry"),
                   ("timed", setField Entry "expires" (v "timed") (some_ tTime (v "at")))] <|
              tup [call "put" [v "store", v "key", v "timed"], mkC Reply "Count" [int 1]])
            (tup [v "store", failure "invalid expire time"])))
        (tup [v "store", mkC Reply "Count" [int 0]])

/-- ```
fn without_expired(store: Store, now: Time) : Store
  var kept = store
  for (key, entry) in store.entries
    if entry.expires is Some(at) and at <= now
      kept.entries = kept.entries.remove(key)
    end
  end
  kept
end
```
Iterating a map visits its `(key, value)` entries in insertion order. -/
def withoutExpired : FunDecl :=
  fn "without_expired" [("store", Store.ty), ("now", tTime)] Store.ty <|
    lets [("kept", v "store")] <|
      forList "w" (tTup [tText, Entry.ty]) "%ke" (emop .entries [entries (v "store")])
        [("kept", Store.ty)] [v "kept"]
        (lets [("key", fst (v "%ke")), ("entry", snd (v "%ke"))] <|
          caseOpt (get Entry "expires" (v "entry")) "at"
            (ite (cmp .time .le (v "at") (v "now"))
              (nextRound "w" [setField Store "entries" (v "kept") (emop .remove [entries (v "kept"), v "key"])])
              (nextRound "w" [v "kept"]))
            (nextRound "w" [v "kept"]))
        (v "kept")

/-- ```
fn put(store: Store, key: Text, entry: Entry) : Store
  Store(entries: store.entries.set(key, entry))
end
``` -/
def put : FunDecl :=
  fn "put" [("store", Store.ty), ("key", tText), ("entry", Entry.ty)] Store.ty <|
    mk Store [emop .set [entries (v "store"), v "key", v "entry"]]

/-- ```
fn main(console: Console, clock: Clock)
  var store = empty_store
  while console.read_line is Some(line)
    reply = case parse(line)
      Ok(command):
        (next, answer) = apply(store, command, clock.now)
        store = next
        answer
      Error(problem): Failure(message: describe(problem))
    end
    console.print("#{reply}")
  end
end
```
`reply = case ...` assigns `store` in one arm, so the join point for the rest
of the loop body takes both `reply` and `store`. -/
def mainFn : FunDecl :=
  fn "main" [("console", tCap .console), ("clock", tCap .clock)] tUnit <|
    lets [("store", global "empty_store")] <|
      whileLoop "serve" [("store", Store.ty)] [v "store"]
        (caseOpt (io .readLine [v "console"]) "line"
          (.join "%reply" [("reply", Reply.ty), ("store", Store.ty)]
            (seq (print (v "console") (call "Reply.to_text" [v "reply"])) (nextWhile "serve" [v "store"]))
            (caseRes (call "parse" [v "line"])
              "command"
                (lets [("%r", call "apply" [v "store", v "command", io .now [v "clock"]])] <|
                  lets [("next", fst (v "%r")), ("answer", snd (v "%r"))] <|
                    .jump "%reply" [v "answer", v "next"])
              "problem" (.jump "%reply" [mkC Reply "Failure" [call "describe" [v "problem"]], v "store"])))
          (breakLoop "serve" [v "store"]))
        unitE

/-- ```
fn describe(problem: ParseError) : Text
  case problem
    Empty: "no command"
    UnknownCommand(name: name): "unknown command #{name}"
    WrongArguments(command: name): "wrong number of arguments for #{name}"
    NotAnInteger(text: text): "#{text} is not an integer"
  end
end
``` -/
def describe : FunDecl :=
  fn "describe" [("problem", ParseError.ty)] tText <|
    .case_ (v "problem")
      [("Empty", [], str "no command"),
       ("UnknownCommand", ["name"], cat [str "unknown command ", v "name"]),
       ("WrongArguments", ["name"], cat [str "wrong number of arguments for ", v "name"]),
       ("NotAnInteger", ["text"], cat [v "text", str " is not an integer"])]

/-- ```
fn holding(key: Text, value: Text, expires: Option(Time)) : Store
  Store(entries: {key: Entry(value: value, expires: expires)})
end
``` -/
def holding : FunDecl :=
  fn "holding" [("key", tText), ("value", tText), ("expires", tOpt tTime)] Store.ty <|
    mk Store [mapOf tText Entry.ty [(v "key", mk Entry [v "value", v "expires"])]]

/-- apply: "a store never holds a key past its expiry"
`result.0.entries.values.all?(fn(entry) entry.expires is Some(at) implies at > now end)` -/
def applyKeepsLive : FunDecl :=
  promiseFn "apply.promise.1" [("store", Store.ty), ("command", Command.ty), ("now", tTime), ("result", stepTy)]
    [allL Entry.ty (emop .values [entries (fst (v "result"))])
      (lam [("entry", Entry.ty)]
        (caseOpt (get Entry "expires" (v "entry")) "at" (cmp .time .gt (v "at") (v "now")) tt))]

def program : Program :=
  wholeProgram "Kv" "main" [Entry, Store, Command, Reply, ParseError] [maxExpiry, emptyStore, t0]
    [replyText, parse, buildCommand, apply, increment, expire,
     withoutExpired, put, mainFn, describe, holding, applyKeepsLive, Lib.all Entry.ty]

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

def parsed (s : String) : Expr := call "parse" [str s]
def errP (c : String) (args : List Expr) : Expr := err_ Command.ty ParseError.ty (mkC ParseError c args)
def holdingE (k val : String) : Expr := call "holding" [str k, str val, none_ tTime]
def t0E : Expr := global "t0"

-- parse("SET name ada") == Ok(Put(key: "name", value: "ada"))
#guard passes program (eq cmdRes (parsed "SET name ada") (okC (mkC Command "Put" [str "name", str "ada"])))
-- parse("get name") == Ok(Get(key: "name"))
#guard passes program (eq cmdRes (parsed "get name") (okC (mkC Command "Get" [str "name"])))
-- parse("DEL a b") == Ok(Del(keys: ["a", "b"]))
#guard passes program (eq cmdRes (parsed "DEL a b") (okC (mkC Command "Del" [strs ["a", "b"]])))
-- parse("EXPIRE name ten") == Error(NotAnInteger(text: "ten"))
#guard passes program (eq cmdRes (parsed "EXPIRE name ten") (errP "NotAnInteger" [str "ten"]))
-- parse("GET") == Error(WrongArguments(command: "GET"))
#guard passes program (eq cmdRes (parsed "GET") (errP "WrongArguments" [str "GET"]))
-- parse("FLY away") == Error(UnknownCommand(name: "FLY"))
#guard passes program (eq cmdRes (parsed "FLY away") (errP "UnknownCommand" [str "FLY"]))
-- parse("   ") == Error(Empty)
#guard passes program (eq cmdRes (parsed "   ") (errP "Empty" []))
-- increment(holding("n", "41", None), "n").1 == Count(42)
#guard passes program
  (eq Reply.ty (snd (call "increment" [holdingE "n" "41", str "n"])) (mkC Reply "Count" [int 42]))
-- increment(empty_store, "n").1 == Count(1)
#guard passes program
  (eq Reply.ty (snd (call "increment" [global "empty_store", str "n"])) (mkC Reply "Count" [int 1]))
-- increment(holding("n", "x", None), "n").1 == Failure(message: "the value is not an integer")
#guard passes program
  (eq Reply.ty (snd (call "increment" [holdingE "n" "x", str "n"])) (failure "the value is not an integer"))
-- increment(holding("n", "9223372036854775807", None), "n").1 == Failure(message: "the increment would overflow")
#guard passes program
  (eq Reply.ty (snd (call "increment" [holdingE "n" "9223372036854775807", str "n"]))
    (failure "the increment would overflow"))
-- expire(empty_store, "a", 10, t0).1 == Count(0)
#guard passes program
  (eq Reply.ty (snd (call "expire" [global "empty_store", str "a", int 10, t0E])) (mkC Reply "Count" [int 0]))
-- expire(holding("a", "1", None), "a", 10, t0).1 == Count(1)
#guard passes program
  (eq Reply.ty (snd (call "expire" [holdingE "a" "1", str "a", int 10, t0E])) (mkC Reply "Count" [int 1]))
-- expire(holding("a", "1", None), "a", -1, t0) == (empty_store, Count(1))
#guard passes program
  (eq stepTy (call "expire" [holdingE "a" "1", str "a", int (-1), t0E])
    (tup [global "empty_store", mkC Reply "Count" [int 1]]))
-- expire(holding("a", "1", None), "a", 4_000_000_000, t0).1 == Failure(message: "invalid expire time")
#guard passes program
  (eq Reply.ty (snd (call "expire" [holdingE "a" "1", str "a", int 4000000000, t0E]))
    (failure "invalid expire time"))

/-! ### The promise's anchors -/

def putA : Expr := mkC Command "Put" [str "a", str "1"]
-- allows store: empty_store, command: Put(key: "a", value: "1"), now: t0, result: (holding("a", "1", None), Okay)
#guard passes program
  (promiseHolds "apply.promise.1" [global "empty_store", putA, t0E, tup [holdingE "a" "1", mkC Reply "Okay" []]])
-- forbids ... result: (holding("a", "1", Some(t0)), Okay)
#guard passes program
  (not_ (promiseHolds "apply.promise.1" [global "empty_store", putA, t0E,
    tup [call "holding" [str "a", str "1", some_ tTime t0E], mkC Reply "Okay" []]]))

/-! ### Test "a key expires after its time"

```
(with_key, _) = apply(empty_store, Put(key: "session", value: "abc"), t0)
(timed, set) = apply(with_key, Expire(key: "session", seconds: 10), t0)
assert set == Count(1)
(_, before) = apply(timed, Get(key: "session"), t0 + 9.seconds)
(_, after) = apply(timed, Get(key: "session"), t0 + 10.seconds)
assert before == Value("abc")
assert after == NoValue
``` -/
#guard passes program
  (lets [("with_key", fst (call "apply" [global "empty_store", mkC Command "Put" [str "session", str "abc"], t0E])),
         ("%r", call "apply" [v "with_key", mkC Command "Expire" [str "session", int 10], t0E]),
         ("timed", fst (v "%r")), ("set", snd (v "%r"))] <|
    and_ (eq Reply.ty (v "set") (mkC Reply "Count" [int 1])) <|
    lets [("before", snd (call "apply" [v "timed", mkC Command "Get" [str "session"],
            prim (.time .add) [t0E, iop .seconds [int 9]]])),
          ("after", snd (call "apply" [v "timed", mkC Command "Get" [str "session"],
            prim (.time .add) [t0E, iop .seconds [int 10]]]))] <|
      and_ (eq Reply.ty (v "before") (mkC Reply "Value" [str "abc"]))
        (eq Reply.ty (v "after") (mkC Reply "NoValue" [])))

/-! ### Test "the console session prints each reply"

```
console = Console.fake_input(["SET a 1", "INCR a", "GET a", "FLY"])
main(console, Clock.fixed(t0))
assert console.printed == ["OK", "(integer) 2", "\"2\"", "(error) unknown command FLY"]
``` -/
#guard passes program
  (lets [("console", io .consoleFakeInput [strs ["SET a 1", "INCR a", "GET a", "FLY"]])] <|
   seq (call "main" [v "console", io .clockFixed [t0E]]) <|
    eq (tList tText) (io .printed [v "console"]) (strs ["OK", "(integer) 2", "\"2\"", "(error) unknown command FLY"]))

/-! ### Never wrong -/

/-- Running `main` never goes wrong, whatever the commands on its input. -/
theorem main_never_wrong (n : Nat) (w : World) (msg : String) :
    evalProgram n program (handles [("console", .console), ("clock", .clock)]) w
      (call "main" [v "console", v "clock"]) ≠ .wrong msg :=
  never_wrong_with_handles (τ := tUnit) checked (by decide +kernel) n w msg

end MoCore.Examples.Kv

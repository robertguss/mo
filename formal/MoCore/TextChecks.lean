import MoCore.Text.Run
import MoCore.Builtins
import MoCore.Examples.Greet
import MoCore.Examples.Wc
import MoCore.Examples.Find
import MoCore.Examples.Expenses
import MoCore.Examples.JsonQuery
import MoCore.Examples.Todo
import MoCore.Examples.Account
import MoCore.Examples.RateLimit
import MoCore.Examples.JobQueue
import MoCore.Examples.Kv
import MoCore.Examples.NotesTool
import MoCore.Examples.Inventory

namespace MoCore.TextChecks
open Text Examples

def programs : List Program :=
  [Greet.program, Wc.program, Find.program, Expenses.program, JsonQuery.program, Todo.program,
   Account.program, RateLimit.program, JobQueue.program, Kv.program, NotesTool.program, Inventory.program]

#guard programs.all programRoundTrip
#guard [Builtins.program, Builtins.layered, Builtins.stoppedInit, Builtins.fakeProgram].all programRoundTrip

#guard
  let ts : List Ty := (tyAtoms.map Prod.snd) ++ (capTable.map fun (_, c) => Ty.cap c) ++
    [.tuple [], .list .float, .map .text .byte, .set .int, .data "T", .option .char,
     .result .unit .fileError, .fn [.bytes, .range] .duration]
  ts.all fun t =>
    match parseDocument (document (tyOut t)) >>= tyIn with
    | .ok u => t == u && document (tyOut t) == document (tyOut u)
    | _ => false

-- Every primitive, including ones none of the samples uses.
def primitives : List Prim :=
  (intOps.map fun (_, o) => .int o) ++ (bigOps.map fun (_, o) => .big o) ++
  (byteOps.map fun (_, o) => .byte o) ++ (decOps.map fun (_, o) => .dec o) ++
  (floatOps.map fun (_, o) => .float o) ++ (charOps.map fun (_, o) => .char o) ++
  (textOps.map fun (_, o) => .text o) ++ (bytesOps.map fun (_, o) => .bytes o) ++
  (rangeOps.map fun (_, o) => .range o) ++ (timeOps.map fun (_, o) => .time o) ++
  (bases.flatMap fun (_, b) => cmpOps.map fun (_, o) => .cmp b o) ++
  (setOps.map fun (_, o) => .set (.option .float) o) ++
  (mapOps.map fun (_, o) => .map .text .bigInt o) ++
  ([ListOp.lit 3, .size, .isEmpty, .get, .first, .last, .append, .prepend, .concat, .reverse,
    .take, .drop, .set, .removeAt, .uncons, .enumerate, .contains, .unique, .zip .char].map fun o => .list .byte o) ++
  (ioOps.map fun (_, o) => .io o) ++
  ([false, true].flatMap fun ro => [Prim.io (.readText ro), .io (.readBytes ro), .io (.list ro),
    .io (.«exists» ro), .io (.within ro)]) ++ [.not, .eq (.tuple [.int, .char]), .map .int .text (.lit 2)]

#guard primitives.all fun p =>
  match parseDocument (document (primOut p)) >>= primIn with
  | .ok q => reprStr p == reprStr q && document (primOut p) == document (primOut q)
  | _ => false

def values : List Value :=
  [(.int (-9223372036854775808)), .bigInt (10^100), .byte 255, .decimal (-17),
   .float 0xfff8000000000001, .bool false, .char '\'', .text "\n\"β", .bytes [0, 255],
   .unit, .range (-2) 9, .time (-1), .duration 123,
   .tuple [.int 3, .char 'a'], .list [.bool true], .map [(.text "k", .byte 2)],
   .set [.int 2, .int 1], .con "C" [.text "f"],
   .closure [("x", .float 0x8000000000000000)] [("p", .byte)] (.global "g") [("g", .byte 7)],
   .cap .files none, .cap .readFiles (some "notes"),
   .cap .console (some "") (some 0), .cap .files none (some 1),
   .cap .readFiles (some "notes") (some 1)]
def world : World :=
  { args := ["a", ""], env := [("Z", "last"), ("A", "first")], stdin := ["one", "two"],
    stdout := ["old"], stderr := ["err"], files := [("b", "β\n"), ("a", "")], now := -1, seed := 10^40,
    fakes := [{ stdin := ["fake input"], stdout := ["fake output"] },
      { files := [("notes/a", "first"), ("z", "last")], seed := 123 }] }

#guard values.all fun v =>
  match parseDocument (document (valueOut v)) >>= valueIn with
  | .ok u => reprStr v == reprStr u && document (valueOut v) == document (valueOut u)
  | _ => false
#guard match parseDocument (document (worldOut world)) >>= worldIn with
  | .ok w => reprStr w == reprStr world && document (worldOut w) == document (worldOut world)
  | _ => false
#guard ([Outcome.ok (.list values) world, .stopped (.overflow "Int") world,
  .stopped .divByZero world, .stopped (.stop "reason") world, .stopped (.outside "call") world,
  .stopped .repeatedKey world, .failed "detail" world, .jumped "j" values world, .wrong "internal", .outOfFuel]).all fun o =>
  match parseDocument (document (outcomeOut o)) >>= outcomeIn with
  | .ok u => reprStr o == reprStr u && document (outcomeOut o) == document (outcomeOut u)
  | _ => false

-- Independent expected spellings: insertion order and escaping must stay visible.
#guard (structural Greet.program (.map .text (.set .float))
  (.map [(.text "a\n", .set [.float 0x8000000000000000, .float 0x7ff8000000000001])])).toOption ==
  some "{\"a\\n\": [0x8000000000000000, 0x7ff8000000000001].to_set}"
#guard (structural Account.program (.data "Account")
  (.con "Account" [.text "alice", .decimal (1500000000000000000)])).toOption ==
  some "Account(owner: \"alice\", balance: 1.5)"
#guard (structural JsonQuery.program JsonQuery.J (.con "String" [.text "é"])).toOption == some "String(\"é\")"
#guard (structural Greet.program (.cap .readFiles) (.cap .readFiles (some "notes") (some 1))).toOption ==
  some "(cap (readFiles) (some \"notes\") (some 1))"
#guard (parseSexp "(cap (console) (some \"\"))" >>= valueIn).toOption.isNone
#guard (parseSexp "(World () () () () () () 0 0)" >>= worldIn).toOption.isNone
#guard (parseSexp "(effects () () ())" >>= effectsIn).toOption.isNone
#guard (structural Greet.program (.tuple [.char, .text, .duration, .time, .range, .bytes])
  (.tuple [.char '\'', .text "\n", .duration 1500000, .time 0, .range (-2) 3, .bytes [0, 255]])).toOption ==
  some "('\\'', \"\\n\", 1.5s, 1970-01-01T00:00:00Z, -2..3, bytes[0, 255])"

#guard (["", "(core 0.1 (unit))", "(core 0.3 (unit))", "(core 0.2 (unit)) extra", "(core 0.2 (unit)",
  "(core 0.2 \"\\q\")", "(core 0.2 \"\\u{d800}\")", "(core 0.2 \"\\u{110000}\")",
  "(core 0.2 \"\\u{}\")", "(core 0.2 \"unterminated)"]).all fun s => (parseDocument s).toOption.isNone
#guard (parseDocument ";before\n(core ;version\n0.2 (int -00012));after" >>= litIn).toOption == some (.int (-12))
#guard (parseDocument "(core 0.2 (float 0xFFFFFFFFFFFFFFFF))" >>= litIn).toOption == some (.float 0xffffffffffffffff)
#guard (["(float 0x123)", "(float 0x10000000000000000)", "(char \"ab\")", "(unit extra)",
  "(int +1)", "(int 1_0)"]).all fun s => (parseSexp s >>= litIn).toOption.isNone

def cap (c : Cap) : Value := .cap c (some "")
def cliRun (p : Program) (w : World) : Run :=
  ⟨p, w, (automaticEntry p).toOption.getD (.entry []), 10000⟩

/-- Reproducible timing inputs; no front-end or generated wrapper is involved. -/
def samples : List (String × Run) :=
  [("Greet", cliRun Greet.program { args := ["Ada", "Grace"] }),
   ("Wc", cliRun Wc.program
     { args := ["a.txt", "gone.txt", "b.txt"], files := [("a.txt", "one two\n"), ("b.txt", "three\n")] }),
   ("Find", cliRun Find.program
     { args := ["-i", "needle", "a.txt"], files := [("a.txt", "Needle\nother\n")] }),
   ("Expenses", cliRun Expenses.program { args := ["october.csv"], files := [("october.csv",
     "date,category,amount\n2026-10-01,food,12.50\n2026-10-02,food,x\n2026-10-03,rent,900\n")] }),
   ("JsonQuery", cliRun JsonQuery.program { args := ["items.0.name"], stdin := ["{\"items\": [{\"name\": \"pen\"}]}"] }),
   ("Todo", cliRun Todo.program { args := ["add", "write the codec"] }),
   ("Account", ⟨Account.program, {}, .entry [.con "Account" [.text "alice", .decimal (100*decScale)],
     .con "Account" [.text "bob", .decimal (20*decScale)], .decimal (5*decScale)], 10000⟩),
   ("RateLimit", ⟨RateLimit.program, {}, .entry [.con "Limiter" [.decimal (10*decScale),
     .decimal (2*decScale), .map []], .text "client", .time 0], 10000⟩),
   ("JobQueue", ⟨JobQueue.program, {}, .entry [.con "Queue" [.map [], .int 1], .time 0, .duration 1000000], 10000⟩),
   ("Kv", cliRun Kv.program { stdin := ["SET a 1", "INCR a", "GET a", "FLY"] }),
   ("NotesTool", ⟨NotesTool.program, { files := [("a.md", "Needle\n"), ("b.txt", "needle\n")] },
     .entry [cap .readFiles, .con "SearchInput" [.text "needle", .int 10]], 10000⟩),
   ("Inventory", cliRun Inventory.program {}),
   ("Fakes", ⟨Builtins.fakeProgram, {}, .test "fakes", 10000⟩)]

#guard samples.all fun (_, r) =>
  match parseDocument (document (runOut r)) >>= runIn with
  | .ok s => reprStr r == reprStr s && document (runOut r) == document (runOut s) &&
      reprStr (execute r) == reprStr (execute s) &&
      match execute s with | .returned .. => true | _ => false
  | _ => false
#guard samples.all fun (_, r) =>
  let report := execute r
  match parseDocument (document (reportOut report)) >>= reportIn with
  | .ok q => reprStr report == reprStr q && document (reportOut q) == document (reportOut report)
  | _ => false

#guard [Report.returned "raw\ntext" (effects world), .stopped .repeatedKey (effects world),
  .failed "detail" (effects world), .outOfFuel, .wrong "internal", .parseError "syntax",
  .illTyped "type", .invalidRun "selection", .ioError "host"].all fun r =>
  match parseDocument (document (reportOut r)) >>= reportIn with
  | .ok s => reprStr r == reprStr s && document (reportOut r) == document (reportOut s)
  | _ => false

#guard match execute ⟨Builtins.layered, {}, .test "bad assertion", 30⟩ with
  | .failed "expected 14" _ => true | _ => false
#guard match execute ⟨Builtins.layered, {}, .test "good example", 30⟩ with
  | .returned "Unit" _ => true | _ => false
#guard match execute ⟨Builtins.stoppedInit, {}, .entry [cap .console], 30⟩ with
  | .stopped (.stop "initialization stopped") _ => true | _ => false
#guard match execute ⟨Greet.program, {}, .entry [cap .args, cap .console], 0⟩ with
  | .outOfFuel => true | _ => false
#guard match execute ⟨{Greet.program with entry := "missing"}, {}, .entry [], 10⟩ with
  | .illTyped _ => true | _ => false
#guard match execute ⟨Greet.program, {}, .entry [.int 0, cap .console], 10⟩ with
  | .invalidRun _ => true | _ => false
#guard match execute ⟨Greet.program, {}, .entry [cap .args, .cap .console (some "") (some 0)], 30⟩ with
  | .invalidRun _ => true | _ => false
#guard match execute ⟨Greet.program, { fakes := [{ args := ["Ada"] }, {}] },
    .entry [.cap .args (some "") (some 0), .cap .console (some "") (some 1)], 30⟩ with
  | .returned "Unit" e => e.stdout.isEmpty && (e.fakes[1]?.map (·.stdout)) == some ["Hello, Ada!"]
  | _ => false
#guard !valueFits Greet.program {} (.list (.cap .console)) (.list [.cap .console none (some 0)])

end MoCore.TextChecks

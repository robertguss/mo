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
  (unicodeOps.map fun (_, o) => .unicode16 o) ++
  (rangeOps.map fun (_, o) => .range o) ++ (timeOps.map fun (_, o) => .time o) ++
  (bases.flatMap fun (_, b) => cmpOps.map fun (_, o) => .cmp b o) ++
  (setOps.map fun (_, o) => .set (.option .float) o) ++
  (mapOps.map fun (_, o) => .map .text .bigInt o) ++
  ([ListOp.lit 3, .size, .isEmpty, .get, .first, .last, .append, .prepend, .concat, .reverse,
    .take, .drop, .set, .removeAt, .uncons, .enumerate, .contains, .unique, .zip .char].map fun o => .list .byte o) ++
  (ioOps.map fun (_, o) => .io o) ++
  ([false, true].flatMap fun ro => [Prim.io (.readText ro), .io (.readBytes ro), .io (.list ro),
    .io (.entries ro), .io (.«exists» ro), .io (.within ro)]) ++ [.not, .eq (.tuple [.int, .char]), .map .int .text (.lit 2)]

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
    stdout := ["old"], stderr := ["err"], files := [("b", "β\n"), ("a", "")], now := -1,
    random := ⟨0, 0xffffffffffffffff, 0x8000000000000000, 42⟩,
    folders := ["empty/β", "empty", "empty"],
    fakes := [{ stdin := ["fake input"], stdout := ["fake output"] },
      { files := [("notes/a", "first"), ("z", "last")], folders := ["notes", "fake empty"],
        random := RandomState.seeded 123 }] }

#guard values.all fun v =>
  match parseDocument (document (valueOut v)) >>= valueIn with
  | .ok u => reprStr v == reprStr u && document (valueOut v) == document (valueOut u)
  | _ => false
#guard match parseDocument (document (worldOut world)) >>= worldIn with
  | .ok w => reprStr w == reprStr world && document (worldOut w) == document (worldOut world)
  | _ => false
#guard ([Outcome.ok (.list values) world, .stopped (.overflow "Int") world,
  .stopped .divByZero world, .stopped (.stop "reason") world, .stopped (.outside "call") world,
  .stopped .repeatedKey world, .stopped (.fileFolderConflict "a/β") world,
  .failed "detail" world, .jumped "j" values world, .wrong "internal", .outOfFuel]).all fun o =>
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

#guard (["", "(core 0.1 (unit))", "(core 0.2 (unit))", "(core 0.3 (unit))", "(core 0.4 (unit))",
  "(core 0.5 (unit))", "(core 0.6 (unit))", "(core 0.8 (unit))", "(core 0.7 (unit)) extra", "(core 0.7 (unit)",
  "(core 0.7 \"\\q\")", "(core 0.7 \"\\u{d800}\")", "(core 0.7 \"\\u{110000}\")",
  "(core 0.7 \"\\u{}\")", "(core 0.7 \"unterminated)"]).all fun s => (parseDocument s).toOption.isNone
#guard (parseDocument ";before\n(core ;version\n0.7 (int -00012));after" >>= litIn).toOption == some (.int (-12))
#guard (parseDocument "(core 0.7 (float 0xFFFFFFFFFFFFFFFF))" >>= litIn).toOption == some (.float 0xffffffffffffffff)
#guard (["(float 0x123)", "(float 0x10000000000000000)", "(char \"ab\")", "(unit extra)",
  "(int +1)", "(int 1_0)"]).all fun s => (parseSexp s >>= litIn).toOption.isNone

def cap (c : Cap) : Value := .cap c (some "")
def cliRun (p : Program) (w : World) : Run :=
  ⟨p, w, (automaticEntry p).toOption.getD (.entry []), 10000⟩

/-- Synthetic source provenance, not a claimed translation of a Mo file. -/
def metadataExpr : Expr :=
  .let_ "x#0" (.global "seed")
    (.let_ "f#0" (.lam [("y#0", .int)] (.prim (.int .add) [.var "x#0", .var "y#0"]))
      (.join "done" [("z#0", .int)]
        (.case_ (.con (.option .int) "Some" [.var "z#0"])
          [("None", [], .lit (.int 0)), ("Some", ["q#0"], .var "q#0")])
        (.jump "done" [.app (.var "f#0") [.var "x#0"]])))

def metadataMeaning : Program :=
  { data := [⟨"Token", [⟨"Token", [("value", .int)]⟩]⟩,
             ⟨"Visible", [⟨"Visible", []⟩]⟩],
    funs := [
      { name := "make", params := [("n#0", .int)], ret := .data "Token",
        body := .con (.data "Token") "Token" [.var "n#0"], module := "Hidden", constCount := 1 },
      { name := "entry", params := [], ret := .int, body := metadataExpr, module := "Main", constCount := 1 },
      { name := "test", params := [], ret := .unit, body := .lit .unit, module := "Main", kind := .test },
      { name := "example", params := [], ret := .unit, body := .lit .unit, module := "Main", kind := .example },
      { name := "promise", params := [("n#1", .int), ("result#0", .data "Token")], ret := .int,
        body := .lit (.int 0), module := "Hidden", kind := .promise },
      { name := "allows", params := [], ret := .int, body := .lit (.int 0), module := "Hidden", kind := .anchor },
      { name := "forbids", params := [], ret := .int, body := .lit (.int 1), module := "Hidden", kind := .anchor }],
    consts := [⟨"seed", .int, .lit (.int 6)⟩], entry := "entry" }

def fixtureSpan (file start finish : Nat) : Option SourceSpan := some ⟨file, start, finish⟩
def fixtureBody (n : Nat) (ps : List (Option String) := []) : BodyMeta :=
  ⟨(List.range n).map (fun i => fixtureSpan (i % 2) (10 + 3*i) (12 + 3*i)), ps, [], []⟩
def metadataFixture : Program :=
  { metadataMeaning with metadata := (.source ["fixtures/hidden.mo", "fixtures/main.mo"]
    [⟨"Hidden", [0], [0, 4, 5, 6], [0], [(0, false)], [0]⟩,
     ⟨"Main", [1], [1, 2, 3], [], [(1, true)], [1]⟩]
    [⟨"Hidden.make", fixtureSpan 0 0 40, none, fixtureBody 2 [some "n"], none, none, []⟩,
     ⟨"Main.main", fixtureSpan 1 0 90, none,
       { (fixtureBody 17) with
         bindings := [⟨0, 0, "x"⟩, ⟨2, 0, "f"⟩, ⟨3, 0, "y"⟩, ⟨7, 0, "z"⟩, ⟨8, 0, "q"⟩]
         closures := [(3, "Main.main.fn.1")] }, none, none, []⟩,
     ⟨"Main.test.1", fixtureSpan 1 100 130, some "a sentence with β and \"quotes\"", fixtureBody 1, none, none, []⟩,
     ⟨"Main.main.example.1", fixtureSpan 1 140 160, some "main() == 12", fixtureBody 1, none, none, []⟩,
     ⟨"Hidden.make.promise.1", fixtureSpan 0 50 90, some "promise \"keeps n\"",
       fixtureBody 1 [some "n", some "result"], some 0, none,
       [⟨fixtureSpan 0 60 70, "result.value == n"⟩, ⟨fixtureSpan 0 71 80, "n >= 0"⟩]⟩,
     ⟨"Hidden.make.promise.1.allows.1", fixtureSpan 0 91 100, some "allows n: 0, result: Token(0)",
       fixtureBody 1, some 4, some true, []⟩,
     ⟨"Hidden.make.promise.1.forbids.1", none, some "forbids n: 1, result: Token(0)",
       { (fixtureBody 1) with spans := [none] }, some 4, some false, []⟩]
    [⟨"Hidden.seed", fixtureSpan 0 110 118, fixtureBody 1⟩]) }

#guard metadataValid metadataFixture
#guard programRoundTrip metadataFixture
#guard (exprNodes metadataExpr).map binderSlots == [1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0]
#guard reprStr (execute ⟨metadataFixture, {}, .entry [], 100⟩) ==
  reprStr (execute ⟨metadataMeaning, {}, .entry [], 100⟩)
#guard match execute ⟨metadataFixture, {}, .entry [], 100⟩ with
  | .returned "12" _ => true | _ => false

-- Reject broken references/coverage through the public decoder, not private predicates.
#guard ([
  ("SourceSpan 0 0 40", "SourceSpan 2 0 40"),
  ("SourceSpan 0 0 40", "SourceSpan 0 41 40"),
  ("SourceSpan 0 0 40", "SourceSpan 0 0 4294967296"),
  ("(some (SourceSpan 1 13 15))", ""),
  ("((some \"n\"))", "()"),
  ("BindingMeta 0 0 \"x\"", "BindingMeta 0 1 \"x\""),
  ("BindingMeta 2 0 \"f\"", "BindingMeta 0 0 \"f\""),
  ("BindingMeta 2 0 \"f\"", "BindingMeta 1 0 \"f\""),
  ("((3 \"Main.main.fn.1\"))", "()"),
  ("((3 \"Main.main.fn.1\"))", "((4 \"Main.main.fn.1\"))"),
  ("(0 4 5 6)", "(0 4 5)"),
  ("((0 false))", "((1 false))"),
  ("((0 false))", "((0 false) (0 true))"),
  ("((1 true)) (1)", "((1 true)) (2)"),
  ("(some 0) (none)", "(some 4) (none)"),
  ("(some 4) (some true)", "(some 0) (some true)"),
  ("(some 4) (some false)", "(some 4) (none)"),
  ("(some \"main() == 12\")", "(none)"),
  ("\"fixtures/main.mo\"", "\"fixtures/main\\u{0}.mo\"")
  ] : List (String × String)).all fun (old, replacement) =>
    let text := (printProgram metadataFixture).replace old replacement
    text != printProgram metadataFixture && (parseProgram text).toOption.isNone

-- Source positions, wording and exposure change the document, never run results.
#guard ([
  ("SourceSpan 0 0 40", "SourceSpan 1 7 4294967295"),
  ("a sentence with β", "a different sentence with γ"),
  ("((0 false))", "((0 true))")
  ] : List (String × String)).all fun (old, replacement) =>
    match parseProgram ((printProgram metadataFixture).replace old replacement) with
    | .ok p => programRoundTrip p && printProgram p != printProgram metadataFixture &&
        [(.entry [], 100), (.test "test", 100), (.entry [], 0)].all fun (selection, fuel) =>
          reprStr (execute ⟨p, {}, selection, fuel⟩) ==
          reprStr (execute ⟨metadataMeaning, {}, selection, fuel⟩)
    | _ => false

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
   ("Fakes", ⟨Builtins.fakeProgram, {}, .test "fakes", 10000⟩),
   ("Library", ⟨Builtins.libraryProgram, {}, .test "library", 10000⟩),
   ("Metadata", ⟨metadataFixture, {}, .entry [], 100⟩)]

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
  .stopped (.fileFolderConflict "a/β") (effects world),
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

/-- Canonical fuzz documents are generated by Lean too, rather than edited by hand. -/
def fuzzSeeds : List (String × Sexp) :=
  [("type", tyOut (.tuple [.entry, .entryKind, .fn [.int, .text] .bool,
    .map .text (.list .byte), .cap .readFiles])),
   ("literal", litOut (.text "\x00\x1f\x7f\n\r\t\"\\β😀")),
   ("value", valueOut (.tuple values)),
   ("primitive", primOut (.list .byte (.zip .text))),
   ("io", primOut (.io (.readBytes true))),
   ("fake_io", primOut (.io .consoleFakeInput)),
   ("program", programOut Builtins.libraryProgram),
   ("metadata", programOut metadataFixture),
   ("outcome", outcomeOut (.jumped "loop" [.int 2, .text "next"] world)),
   ("report", reportOut (.returned "[\"a\", \"b\\n\"]" (effects world))),
   ("cap_state", stateOut world.toCapState),
   ("random_state", randomOut ⟨0, 0x8000000000000000, 0xffffffffffffffff, 0x0123456789abcdef⟩),
   ("unicode16", primOut (.unicode16 .normalizeNfc)),
   ("entries", primOut (.io (.entries true)))]

#guard (structural Greet.program (.list .entry)
  (.list [.con "Entry" [.text "é", .con "Folder" []]])).toOption ==
  some "[Entry(name: \"é\", kind: Folder)]"
#guard ["File", "Folder", "Link", "Other"].all fun c =>
  valueFits Greet.program {} .entryKind (.con c []) &&
  !valueFits Greet.program {} .entryKind (.con c [.text "extra"])
#guard !(valueFits Greet.program {} .entry (.con "Entry" [.text "name", .con "Unknown" []]))
#guard (["(RandomState 0 1 2 3)", "(RandomState 0x0000000000000000)",
  "(RandomState 0x0000000000000000 0x0000000000000000 0x0000000000000000 0x10000000000000000)"]).all
  fun s => (parseSexp s >>= randomIn).toOption.isNone
#guard (parseSexp "(CapState () () () () () () 0 42)" >>= stateIn).toOption.isNone

end MoCore.TextChecks

/-
  examples/notes-tool.mo in Core.

  Exercises: a read-only capability (`ReadFiles`) over an in-memory file map,
  `try` with automatic wrapping into `Unreadable(FileError)`, nested loops,
  `continue`, typed results and errors, and a promise.
-/
import MoCore.Testing

namespace MoCore.Examples.NotesTool

open MoCore DSL Lib

/-- `struct SearchInput  query: Text  limit: Int  end` -/
def SearchInput : DataDecl := record "SearchInput" [("query", tText), ("limit", tInt)]

/-- `struct Hit  file: Text  line: Int  text: Text  end` -/
def Hit : DataDecl := record "Hit" [("file", tText), ("line", tInt), ("text", tText)]

/-- `struct SearchOutput  hits: List(Hit)  truncated?: Bool  end` -/
def SearchOutput : DataDecl := record "SearchOutput" [("hits", tList Hit.ty), ("truncated?", tBool)]

/-- `enum ToolError  EmptyQuery  BadLimit(limit: Int)  Unreadable(FileError)  end` -/
def ToolError : DataDecl :=
  choice "ToolError" [("EmptyQuery", []), ("BadLimit", [("limit", tInt)]), ("Unreadable", [("0", tFileError)])]

def outRes : Ty := tRes SearchOutput.ty ToolError.ty
def limitOf (e : Expr) : Expr := get SearchInput "limit" e
def unreadable (e : Expr) : Expr := mkC ToolError "Unreadable" [e]

/-- `max_limit = 100` -/
def maxLimit : ConstDecl := const "max_limit" tInt (int 100)

/-- ```
fn search_notes(notes: ReadFiles, input: SearchInput) : Result(SearchOutput, ToolError)
  query = input.query.trim.ascii_lowercase
  return Error(EmptyQuery) if query.empty?
  return Error(BadLimit(limit: input.limit)) if input.limit < 1 or input.limit > max_limit
  var hits = []
  for name in try notes.list(".")
    continue if not name.ends_with?(".md")
    text = try notes.read_text(name)
    for (index, line) in text.lines.enumerate
      if line.ascii_lowercase.contains?(query)
        hits = hits.append(Hit(file: name, line: index + 1, text: line.trim))
      end
    end
  end
  Ok(SearchOutput(hits: hits.take(input.limit), truncated?: hits.size > input.limit))
end
```
Both `try`s wrap a `FileError` in `Unreadable`; the second returns from
inside the outer loop. -/
def searchNotes : FunDecl :=
  fn "search_notes" [("notes", tCap .readFiles), ("input", SearchInput.ty)] outRes <|
    lets [("query", top .lower [top .trim [get SearchInput "query" (v "input")]])] <|
    ite (top .isEmpty [v "query"]) (err_ SearchOutput.ty ToolError.ty (mkC ToolError "EmptyQuery" [])) <|
    ite (or_ (ilt (limitOf (v "input")) (int 1)) (igt (limitOf (v "input")) (global "max_limit")))
      (err_ SearchOutput.ty ToolError.ty (mkC ToolError "BadLimit" [limitOf (v "input")])) <|
    lets [("hits", nil Hit.ty)] <|
    tryWrap SearchOutput.ty ToolError.ty (io (.list true) [v "notes", str "."]) "%names" unreadable <|
      forList "files" tText "name" (v "%names") [("hits", tList Hit.ty)] [v "hits"]
        (ite (not_ (top .endsWith [v "name", str ".md"])) (nextRound "files" [v "hits"]) <|
          tryWrap SearchOutput.ty ToolError.ty (io (.readText true) [v "notes", v "name"]) "text" unreadable <|
            forList "lines" (tTup [tInt, tText]) "%il" (lop tText .enumerate [top .lines [v "text"]])
              [("hits", tList Hit.ty)] [v "hits"]
              (lets [("index", fst (v "%il")), ("line", snd (v "%il"))] <|
                ite (top .contains [top .lower [v "line"], v "query"])
                  (nextRound "lines" [lop Hit.ty .append [v "hits",
                    mk Hit [v "name", iadd (v "index") (int 1), top .trim [v "line"]]]])
                  (nextRound "lines" [v "hits"]))
              (nextRound "files" [v "hits"]))
        (ok_ SearchOutput.ty ToolError.ty
          (mk SearchOutput [lop Hit.ty .take [v "hits", limitOf (v "input")],
                            igt (lop Hit.ty .size [v "hits"]) (limitOf (v "input"))]))

/-- `hit = Hit(file: "a.md", line: 1, text: "x")` -/
def hit : ConstDecl := const "hit" Hit.ty (mk Hit [str "a.md", int 1, str "x"])

/-- search_notes: "a search never returns more hits than its limit"
`result is Ok(output) implies output.hits.size <= input.limit` -/
def searchBounded : FunDecl :=
  promiseFn "search_notes.promise.1" [("notes", tCap .readFiles), ("input", SearchInput.ty), ("result", outRes)]
    [caseRes (v "result")
      "output" (ile (lop Hit.ty .size [get SearchOutput "hits" (v "output")]) (limitOf (v "input")))
      "_" tt]

def program : Program :=
  wholeProgram "NotesTool" "search_notes" [SearchInput, Hit, SearchOutput, ToolError] [maxLimit, hit]
    [searchNotes, searchBounded]

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

def input (q : String) (limit : Int) : Expr := mk SearchInput [str q, int limit]
def search (q : String) (limit : Int) : Expr := call "search_notes" [v "notes", input q limit]

/-! ### The promise's anchors (with `notes: Files.in_memory({}).read_only`) -/

-- allows ..., input: SearchInput(query: "x", limit: 1), result: Ok(SearchOutput(hits: [], truncated?: false))
#guard passes program
  (lets [("notes", io .readOnly [io .filesInMemory [mapOf tText tText []]])] <|
   promiseHolds "search_notes.promise.1" [v "notes", input "x" 1,
    ok_ SearchOutput.ty ToolError.ty (mk SearchOutput [nil Hit.ty, ff])])
-- forbids ..., result: Ok(SearchOutput(hits: [hit, hit], truncated?: false))
#guard passes program
  (lets [("notes", io .readOnly [io .filesInMemory [mapOf tText tText []]])] <|
   not_ (promiseHolds "search_notes.promise.1" [v "notes", input "x" 1,
    ok_ SearchOutput.ty ToolError.ty (mk SearchOutput [listOf Hit.ty [global "hit", global "hit"], ff])]))

/-! ### Test "only Markdown notes are searched, and case doesn't matter"

```
notes = Files.in_memory({
  "plans.md": "# Plans\nCall the Bakery on Monday\n",
  "recipes.md": "bread: flour, water, salt\n",
  "bakery.txt": "not a note\n"
}).read_only
assert search_notes(notes, SearchInput(query: "bakery", limit: 10)) == Ok(SearchOutput(
  hits: [Hit(file: "plans.md", line: 2, text: "Call the Bakery on Monday")],
  truncated?: false
))
``` -/
#guard passes program
  (lets [("notes", io .readOnly [io .filesInMemory [mapOf tText tText
    [(str "plans.md", str "# Plans\nCall the Bakery on Monday\n"),
     (str "recipes.md", str "bread: flour, water, salt\n"), (str "bakery.txt", str "not a note\n")]]])] <|
   eq outRes (search "bakery" 10)
    (ok_ SearchOutput.ty ToolError.ty
      (mk SearchOutput [listOf Hit.ty [mk Hit [str "plans.md", int 2, str "Call the Bakery on Monday"]], ff])))

/-! ### Test "too many hits are cut at the limit, and the output says so"

```
notes = Files.in_memory({"a.md": "x\nx\nx\n"}).read_only
assert search_notes(notes, SearchInput(query: "x", limit: 2)) is Ok(output)
assert output.hits.size == 2 and output.truncated?
``` -/
#guard passes program
  (lets [("notes", io .readOnly [io .filesInMemory [mapOf tText tText [(str "a.md", str "x\nx\nx\n")]]])] <|
   caseRes (search "x" 2)
    "output"
      (and_ (ieq (lop Hit.ty .size [get SearchOutput "hits" (v "output")]) (int 2))
        (get SearchOutput "truncated?" (v "output")))
    "_" ff)

/-! ### Test "bad input is a typed error"

```
no_notes = Files.in_memory({}).read_only
assert search_notes(no_notes, SearchInput(query: "  ", limit: 5)) == Error(EmptyQuery)
assert search_notes(no_notes, SearchInput(query: "x", limit: 0)) == Error(BadLimit(limit: 0))
``` -/
#guard passes program
  (lets [("notes", io .readOnly [io .filesInMemory [mapOf tText tText []]])] <|
   and_ (eq outRes (search "  " 5) (err_ SearchOutput.ty ToolError.ty (mkC ToolError "EmptyQuery" [])))
    (eq outRes (search "x" 0) (err_ SearchOutput.ty ToolError.ty (mkC ToolError "BadLimit" [int 0]))))

/-! ### More: a narrowed capability (`files.within("notes").read_only`) sees only its folder -/

#guard passes program
  (lets [("notes", io .readOnly [io .filesInMemory [mapOf tText tText
           [(str "notes/plans.md", str "# Plans\n"), (str "other/plans.md", str "# Plans elsewhere\n")]]]),
         ("narrow", io (.within true) [v "notes", str "notes"])] <|
    eq outRes (call "search_notes" [v "narrow", input "plan" 10])
      (ok_ SearchOutput.ty ToolError.ty
        (mk SearchOutput [listOf Hit.ty [mk Hit [str "plans.md", int 1, str "# Plans"]], ff])))

/-! ### More: `within` a path that would leave the folder reaches nothing

`notes.within("..")` is a capability that reaches nothing (reference 11.3,
14): listing its folder gives `Error(NotAllowed("."))`, which the search wraps
in `Unreadable`, and `exists?` is `false` even for its own folder. A path whose
`..` stays inside the folder is an ordinary path; one that would leave it is
`NotAllowed`. -/

def notAllowed (path : String) : Expr := .con tFileError "NotAllowed" [str path]

#guard passes program
  (lets [("notes", io .readOnly [io .filesInMemory [mapOf tText tText [(str "notes/plans.md", str "# Plans\n")]]]),
         ("outside", io (.within true) [v "notes", str ".."])] <|
    and_ (eq outRes (call "search_notes" [v "outside", input "plan" 10])
           (err_ SearchOutput.ty ToolError.ty (unreadable (notAllowed "."))))
      (not_ (io (.exists true) [v "outside", str "."])))

#guard passes program
  (lets [("notes", io .readOnly [io .filesInMemory [mapOf tText tText [(str "plans.md", str "# Plans\n")]]])] <|
   and_ (eq (tRes tText tFileError) (io (.readText true) [v "notes", str "notes/../plans.md"])
          (ok_ tText tFileError (str "# Plans\n")))
    (eq (tRes tText tFileError) (io (.readText true) [v "notes", str "../plans.md"])
      (err_ tText tFileError (notAllowed "../plans.md"))))

/-! ### Never wrong -/

/-- `search_notes` never goes wrong, for any notes folder and any input. -/
theorem search_never_wrong {ρ : Env}
    (hρ : EnvTy program ρ [("notes", .cap .readFiles), ("input", SearchInput.ty)])
    (n : Nat) (w : World) (msg : String) :
    evalProgram n program ρ w (call "search_notes" [v "notes", v "input"]) ≠ .wrong msg :=
  never_wrong (τ := outRes) checked (by decide +kernel) hρ n w msg

end MoCore.Examples.NotesTool

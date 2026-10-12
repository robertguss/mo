/-
  examples/find.mo in Core.

  Exercises: parsing arguments with `case` on text (literal patterns and
  alternatives lowered to `==` tests), assigning a `var`'s fields, `return`
  from inside a loop, searching text, nested loops, and an exit status.
-/
import MoCore.Testing

namespace MoCore.Examples.Find

open MoCore DSL Lib

/-- `struct Search  phrase: Text  files: List(Text)  ignore_case?: Bool  count_only?: Bool  end` -/
def Search : DataDecl :=
  record "Search" [("phrase", tText), ("files", tList tText), ("ignore_case?", tBool), ("count_only?", tBool)]

/-- `enum UsageError  NoPhrase  NoFiles  UnknownOption(option: Text)  end` -/
def UsageError : DataDecl :=
  choice "UsageError" [("NoPhrase", []), ("NoFiles", []), ("UnknownOption", [("option", tText)])]

def searchRes : Ty := tRes Search.ty UsageError.ty
def hitTy : Ty := tTup [tInt, tText]

/-- ```
fn to_text(problem: UsageError) : Text
  case problem
    NoPhrase: "no phrase to find"
    NoFiles: "no files to search"
    UnknownOption(option: o): "unknown option #{o}"
  end
end
``` -/
def toText : FunDecl :=
  fn "UsageError.to_text" [("problem", UsageError.ty)] tText <|
    .case_ (v "problem")
      [("NoPhrase", [], str "no phrase to find"),
       ("NoFiles", [], str "no files to search"),
       ("UnknownOption", ["o"], cat [str "unknown option ", v "o"])]

/-- ```
fn parse_args(args: List(Text)) : Result(Search, UsageError)
  var search = search_for("", [])
  var rest = []
  for arg in args
    case arg
      "--ignore-case" | "-i": search.ignore_case? = true
      "--count" | "-c": search.count_only? = true
      _ if arg.starts_with?("-"): return Error(UnknownOption(option: arg))
      _: rest = rest.append(arg)
    end
  end
  case rest
    []: Error(NoPhrase)
    [_]: Error(NoFiles)
    [phrase, ..files]:
      search.phrase = phrase
      search.files = files
      Ok(search)
  end
end
``` -/
def parseArgs : FunDecl :=
  fn "parse_args" [("args", tList tText)] searchRes <|
    lets [("search", call "search_for" [str "", nil tText]), ("rest", nil tText)] <|
      forList "a" tText "arg" (v "args") [("search", Search.ty), ("rest", tList tText)] [v "search", v "rest"]
        (ite (or_ (teq (v "arg") (str "--ignore-case")) (teq (v "arg") (str "-i")))
          (nextRound "a" [setField Search "ignore_case?" (v "search") tt, v "rest"]) <|
         ite (or_ (teq (v "arg") (str "--count")) (teq (v "arg") (str "-c")))
          (nextRound "a" [setField Search "count_only?" (v "search") tt, v "rest"]) <|
         ite (top .startsWith [v "arg", str "-"])
          (err_ Search.ty UsageError.ty (mkC UsageError "UnknownOption" [v "arg"]))
          (nextRound "a" [v "search", lop tText .append [v "rest", v "arg"]]))
        (caseOpt (lop tText .uncons [v "rest"]) "%p"
          (lets [("phrase", fst (v "%p")), ("files", snd (v "%p"))] <|
            ite (lop tText .isEmpty [v "files"])
              (err_ Search.ty UsageError.ty (mkC UsageError "NoFiles" []))
              (lets [("search", setField Search "phrase" (v "search") (v "phrase")),
                     ("search", setField Search "files" (v "search") (v "files"))] <|
                ok_ Search.ty UsageError.ty (v "search")))
          (err_ Search.ty UsageError.ty (mkC UsageError "NoPhrase" [])))

/-- ```
fn search_for(phrase: Text, files: List(Text)) : Search
  Search(phrase: phrase, files: files, ignore_case?: false, count_only?: false)
end
``` -/
def searchFor : FunDecl :=
  fn "search_for" [("phrase", tText), ("files", tList tText)] Search.ty <|
    mk Search [v "phrase", v "files", ff, ff]

/-- ```
fn matches(text: Text, search: Search) : List((Int, Text))
  phrase = folded(search.phrase, search.ignore_case?)
  var found = []
  for (index, line) in text.lines.enumerate
    if folded(line, search.ignore_case?).contains?(phrase)
      found = found.append((index + 1, line))
    end
  end
  found
end
``` -/
def matchesFn : FunDecl :=
  fn "matches" [("text", tText), ("search", Search.ty)] (tList hitTy) <|
    lets [("phrase", call "folded" [get Search "phrase" (v "search"), get Search "ignore_case?" (v "search")]),
          ("found", nil hitTy)] <|
      forList "l" hitTy "%il" (lop tText .enumerate [top .lines [v "text"]]) [("found", tList hitTy)] [v "found"]
        (lets [("index", fst (v "%il")), ("line", snd (v "%il"))] <|
          ite (top .contains [call "folded" [v "line", get Search "ignore_case?" (v "search")], v "phrase"])
            (nextRound "l" [lop hitTy .append [v "found", tup [iadd (v "index") (int 1), v "line"]]])
            (nextRound "l" [v "found"]))
        (v "found")

/-- `fn folded(text: Text, ignore_case?: Bool) : Text  if ignore_case?: text.ascii_lowercase else: text  end` -/
def folded : FunDecl :=
  fn "folded" [("text", tText), ("ignore_case?", tBool)] tText <|
    ite (v "ignore_case?") (top .lower [v "text"]) (v "text")

/-- ```
fn main(args: Args, console: Console, files: ReadFiles) : Int
  case parse_args(args.list)
    Ok(search): run(search, console, files)
    Error(problem):
      console.error("find: #{problem}")
      console.error("usage: find [--ignore-case] [--count] PHRASE FILE...")
      2
  end
end
``` -/
def mainFn : FunDecl :=
  fn "main" [("args", tCap .args), ("console", tCap .console), ("files", tCap .readFiles)] tInt <|
    caseRes (call "parse_args" [argsList (v "args")])
      "search" (call "run" [v "search", v "console", v "files"])
      "problem"
        (seq (printError (v "console") (cat [str "find: ", call "UsageError.to_text" [v "problem"]])) <|
          seq (printError (v "console") (str "usage: find [--ignore-case] [--count] PHRASE FILE...")) <|
            int 2)

/-- ```
fn run(search: Search, console: Console, files: ReadFiles) : Int
  var matched? = false
  var failed? = false
  for name in search.files
    case files.read_text(name)
      Error(failure):
        console.error("find: #{failure}")
        failed? = true
      Ok(text):
        found = matches(text, search)
        matched? = matched? or not found.empty?
        if search.count_only?
          console.print("#{name}: #{found.size}")
        else
          for (number, line) in found
            console.print("#{name}:#{number}: #{line}")
          end
        end
    end
  end
  if failed?: 2 else if matched?: 0 else: 1
end
``` -/
def run : FunDecl :=
  fn "run" [("search", Search.ty), ("console", tCap .console), ("files", tCap .readFiles)] tInt <|
    lets [("matched?", ff), ("failed?", ff)] <|
      forList "f" tText "name" (get Search "files" (v "search"))
        [("matched?", tBool), ("failed?", tBool)] [v "matched?", v "failed?"]
        (caseRes (io (.readText true) [v "files", v "name"])
          "text"
            (lets [("found", call "matches" [v "text", v "search"]),
                   ("matched?", or_ (v "matched?") (not_ (lop hitTy .isEmpty [v "found"])))] <|
              ite (get Search "count_only?" (v "search"))
                (seq (print (v "console") (cat [v "name", str ": ", intText (lop hitTy .size [v "found"])])) <|
                  nextRound "f" [v "matched?", v "failed?"])
                (forList "m" hitTy "%nl" (v "found") [] []
                  (lets [("number", fst (v "%nl")), ("line", snd (v "%nl"))] <|
                    seq (print (v "console") (cat [v "name", str ":", intText (v "number"), str ": ", v "line"])) <|
                      nextRound "m" [])
                  (nextRound "f" [v "matched?", v "failed?"])))
          "failure"
            (seq (printError (v "console") (cat [str "find: ", fileErrorTextL (v "failure")])) <|
              nextRound "f" [v "matched?", tt]))
        (ite (v "failed?") (int 2) (ite (v "matched?") (int 0) (int 1)))

def program : Program :=
  wholeProgram "Find" "main" [Search, UsageError] []
    [toText, parseArgs, searchFor, matchesFn, folded, mainFn, run, Lib.fileErrorText]

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

def parsed (args : List String) : Expr := call "parse_args" [strs args]
def errU (c : String) (args : List Expr) : Expr := err_ Search.ty UsageError.ty (mkC UsageError c args)

-- parse_args(["cat", "a.txt"]) == Ok(search_for("cat", ["a.txt"]))
#guard passes program
  (eq searchRes (parsed ["cat", "a.txt"]) (ok_ Search.ty UsageError.ty (call "search_for" [str "cat", strs ["a.txt"]])))
-- parse_args(["-i", "--count", "cat", "a.txt"]) is Ok(Search(ignore_case?: true, count_only?: true))
#guard passes program
  (caseRes (parsed ["-i", "--count", "cat", "a.txt"])
    "s" (and_ (eq tBool (get Search "ignore_case?" (v "s")) tt) (eq tBool (get Search "count_only?" (v "s")) tt))
    "_" ff)
-- parse_args(["--fast", "cat", "a.txt"]) == Error(UnknownOption(option: "--fast"))
#guard passes program (eq searchRes (parsed ["--fast", "cat", "a.txt"]) (errU "UnknownOption" [str "--fast"]))
-- parse_args(["cat"]) == Error(NoFiles)
#guard passes program (eq searchRes (parsed ["cat"]) (errU "NoFiles" []))
-- parse_args([]) == Error(NoPhrase)
#guard passes program (eq searchRes (parsed []) (errU "NoPhrase" []))
-- matches("a cat\nno\nCat!", search_for("cat", [])) == [(1, "a cat")]
#guard passes program
  (eq (tList hitTy) (call "matches" [str "a cat\nno\nCat!", call "search_for" [str "cat", nil tText]])
    (listOf hitTy [tup [int 1, str "a cat"]]))
-- matches("", search_for("cat", [])) == []
#guard passes program
  (eq (tList hitTy) (call "matches" [str "", call "search_for" [str "cat", nil tText]]) (nil hitTy))

def caps : List (String × Cap) := [("args", .args), ("console", .console), ("files", .readFiles)]
def runMain : Expr := call "main" [v "args", v "console", v "files"]

/-! ### Test "matching ignores case only when asked"

```
files = Files.in_memory({"pets.txt": "a cat\nCats!\ndog\n"}).read_only
console = Console.fake()
assert main(Args.of(["-i", "cat", "pets.txt"]), console, files) == 0
assert console.printed == ["pets.txt:1: a cat", "pets.txt:2: Cats!"]
``` -/
#guard passes program
  (lets [("args", io .argsOf [strs ["-i", "cat", "pets.txt"]]), ("console", io .consoleFake []),
         ("files", io .readOnly [io .filesInMemory [mapOf tText tText
           [(str "pets.txt", str "a cat\nCats!\ndog\n")]]])] <|
   and_ (ieq runMain (int 0))
    (eq (tList tText) (io .printed [v "console"]) (strs ["pets.txt:1: a cat", "pets.txt:2: Cats!"])))

/-! ### Test "no match gives status 1, and a missing file status 2"

```
files = Files.in_memory({"pets.txt": "dog\n"}).read_only
assert main(Args.of(["cat", "pets.txt"]), Console.fake(), files) == 1
assert main(Args.of(["cat", "gone.txt"]), Console.fake(), files) == 2
``` -/
#guard passes program
  (lets [("files", io .readOnly [io .filesInMemory [mapOf tText tText
    [(str "pets.txt", str "dog\n")]]])] <|
   and_ (ieq (call "main" [io .argsOf [strs ["cat", "pets.txt"]], io .consoleFake [], v "files"]) (int 1))
     (ieq (call "main" [io .argsOf [strs ["cat", "gone.txt"]], io .consoleFake [], v "files"]) (int 2)))

/-! ### More: `--count`, and an unknown option reported on standard error -/

#guard passes program
  (lets [("args", io .argsOf [strs ["-c", "-i", "CAT", "pets.txt", "none.txt"]]),
         ("console", io .consoleFake []),
         ("files", io .readOnly [io .filesInMemory [mapOf tText tText
           [(str "pets.txt", str "a cat\nCats!\ndog\n"), (str "none.txt", str "dog\n")]]])] <|
   and_ (ieq runMain (int 0))
    (eq (tList tText) (io .printed [v "console"]) (strs ["pets.txt: 2", "none.txt: 0"])))

#guard passes program
  (lets [("args", io .argsOf [strs ["--fast", "cat", "pets.txt"]]),
         ("console", io .consoleFake []), ("files", io .readOnly [io .filesInMemory [mapOf tText tText []]])] <|
   and_ (ieq runMain (int 2))
    (eq (tList tText) (io .printedErrors [v "console"])
      (strs ["find: unknown option --fast", "usage: find [--ignore-case] [--count] PHRASE FILE..."])))

/-! ### More: `\n` and `\r\n` end a line, and a `\r` alone stays in its line (reference 14) -/

-- matches("one cat\r\ntwo cats\r", search_for("cat", [])) == [(1, "one cat"), (2, "two cats\r")]
#guard passes program
  (eq (tList hitTy) (call "matches" [str "one cat\r\ntwo cats\r", call "search_for" [str "cat", nil tText]])
    (listOf hitTy [tup [int 1, str "one cat"], tup [int 2, str "two cats\r"]]))

/-! ### Never wrong -/

/-- Running `main` never goes wrong, whatever the arguments and files. -/
theorem main_never_wrong (n : Nat) (w : World) (msg : String) :
    evalProgram n program (handles caps) w runMain ≠ .wrong msg :=
  never_wrong_with_handles (τ := tInt) checked (by decide +kernel) n w msg

end MoCore.Examples.Find

/-
  examples/wc.mo in Core.

  Exercises: reading files, failures, numbers, a `for` loop with two `var`s
  (join points), and the exit status.
-/
import MoCore.Testing

namespace MoCore.Examples.Wc

open MoCore DSL Lib

/-- `struct Counts  lines: Int  words: Int  bytes: Int  end` -/
def Counts : DataDecl := record "Counts" [("lines", tInt), ("words", tInt), ("bytes", tInt)]

/-- ```
fn count(text: Text) : Counts
  Counts(
    lines: text.chars.count(fn(c) c == '\n' end),
    words: text.words.size,
    bytes: text.byte_count
  )
end
``` -/
def count : FunDecl :=
  fn "count" [("text", tText)] Counts.ty <|
    mk Counts [
      countL tChar (top .chars [v "text"]) (lam [("c", tChar)] (eq tChar (v "c") (chr '\n'))),
      lop tText .size [top .words [v "text"]],
      top .byteCount [v "text"]]

/-- ```
fn add(a: Counts, b: Counts) : Counts
  Counts(lines: a.lines + b.lines, words: a.words + b.words, bytes: a.bytes + b.bytes)
end
``` -/
def add : FunDecl :=
  fn "add" [("a", Counts.ty), ("b", Counts.ty)] Counts.ty <|
    mk Counts [
      iadd (get Counts "lines" (v "a")) (get Counts "lines" (v "b")),
      iadd (get Counts "words" (v "a")) (get Counts "words" (v "b")),
      iadd (get Counts "bytes" (v "a")) (get Counts "bytes" (v "b"))]

/-- ```
fn row(counts: Counts, name: Text) : Text
  "#{column(counts.lines)} #{column(counts.words)} #{column(counts.bytes)} #{name}"
end
``` -/
def row : FunDecl :=
  fn "row" [("counts", Counts.ty), ("name", tText)] tText <|
    cat [call "column" [get Counts "lines" (v "counts")], str " ",
         call "column" [get Counts "words" (v "counts")], str " ",
         call "column" [get Counts "bytes" (v "counts")], str " ", v "name"]

/-- ```
fn column(n: Int) : Text
  "#{n}".pad_left(7)
end
``` -/
def column : FunDecl :=
  fn "column" [("n", tInt)] tText <| top .padLeft [intText (v "n"), int 7]

/-- ```
fn main(args: Args, console: Console, files: ReadFiles) : Int
  var total = Counts(lines: 0, words: 0, bytes: 0)
  var failed? = false
  for name in args.list
    case files.read_text(name)
      Ok(text):
        counts = count(text)
        console.print(row(counts, name))
        total = add(total, counts)
      Error(failure):
        console.error("wc: #{failure}")
        failed? = true
    end
  end
  if args.list.size > 1
    console.print(row(total, "total"))
  end
  if failed?: 1 else: 0
end
```
The loop's `var`s, `total` and `failed?`, are the parameters of its join
points. -/
def mainFn : FunDecl :=
  fn "main" [("args", tCap .args), ("console", tCap .console), ("files", tCap .readFiles)] tInt <|
    lets [("total", mk Counts [int 0, int 0, int 0]), ("failed?", ff)] <|
      forList "names" tText "name" (argsList (v "args"))
        [("total", Counts.ty), ("failed?", tBool)] [v "total", v "failed?"]
        (caseRes (io (.readText true) [v "files", v "name"])
          "text"
            (lets [("counts", call "count" [v "text"])] <|
              seq (print (v "console") (call "row" [v "counts", v "name"])) <|
                nextRound "names" [call "add" [v "total", v "counts"], v "failed?"])
          "failure"
            (seq (printError (v "console") (cat [str "wc: ", fileErrorTextL (v "failure")])) <|
              nextRound "names" [v "total", tt]))
        (seq (ite (igt (lop tText .size [argsList (v "args")]) (int 1))
                (print (v "console") (call "row" [v "total", str "total"]))
                unitE) <|
          ite (v "failed?") (int 1) (int 0))

def program : Program :=
  wholeProgram "Wc" "main" [Counts] []
    [count, add, row, column, mainFn, Lib.count tChar, Lib.fileErrorText]

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

-- count("") == Counts(lines: 0, words: 0, bytes: 0)
#guard passes program (eq Counts.ty (call "count" [str ""]) (mk Counts [int 0, int 0, int 0]))
-- count("one two\nthree\n") == Counts(lines: 2, words: 3, bytes: 14)
#guard passes program
  (eq Counts.ty (call "count" [str "one two\nthree\n"]) (mk Counts [int 2, int 3, int 14]))
-- count("no newline") == Counts(lines: 0, words: 2, bytes: 10)
#guard passes program
  (eq Counts.ty (call "count" [str "no newline"]) (mk Counts [int 0, int 2, int 10]))
-- row(Counts(lines: 2, words: 3, bytes: 14), "a.txt") == "      2       3      14 a.txt"
#guard passes program
  (teq (call "row" [mk Counts [int 2, int 3, int 14], str "a.txt"]) (str "      2       3      14 a.txt"))

/-! ### Test "a missing file is reported, and the others still counted"

```
console = Console.fake()
files = Files.in_memory({"a.txt": "one two\n", "b.txt": "three\n"}).read_only
status = main(Args.of(["a.txt", "gone.txt", "b.txt"]), console, files)
assert status == 1
assert console.printed == [
  "      1       2       8 a.txt",
  "      1       1       6 b.txt",
  "      2       3      14 total"
]
assert console.printed_errors == ["wc: gone.txt: no such file"]
``` -/
#guard passes program
  (lets [("status", call "main" [v "args", v "console", v "files"])] <|
    and_ (ieq (v "status") (int 1)) <|
    and_ (eq (tList tText) (io .printed [v "console"])
            (strs ["      1       2       8 a.txt", "      1       1       6 b.txt",
                    "      2       3      14 total"])) <|
    eq (tList tText) (io .printedErrors [v "console"]) (strs ["wc: gone.txt: no such file"]))
  { args := ["a.txt", "gone.txt", "b.txt"], files := [("a.txt", "one two\n"), ("b.txt", "three\n")] }
  [("args", .args), ("console", .console), ("files", .readFiles)]

/-! ### Never wrong -/

/-- Running `main` never goes wrong, whatever the arguments and files. -/
theorem main_never_wrong (n : Nat) (w : World) (msg : String) :
    evalProgram n program (handles [("args", .args), ("console", .console), ("files", .readFiles)]) w
      (call "main" [v "args", v "console", v "files"]) ≠ .wrong msg :=
  never_wrong_with_handles (τ := tInt) checked (by decide +kernel) n w msg

end MoCore.Examples.Wc

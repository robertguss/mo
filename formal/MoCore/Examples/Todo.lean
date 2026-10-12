/-
  examples/todo.mo in Core.

  Exercises: records, commands as a choice type, list patterns with a guard,
  `try` on `Option` and `Result` (with automatic wrapping into
  `Storage(FileError)`), nested patterns on `FileError`, writing files, and a
  test whose fakes are shared by several calls of `main`.
-/
import MoCore.Testing

namespace MoCore.Examples.Todo

open MoCore DSL Lib

/-- `struct Item  text: Text  done?: Bool  end` -/
def Item : DataDecl := record "Item" [("text", tText), ("done?", tBool)]

/-- `enum Command  Add(text: Text)  Show  Done(number: Int)  Remove(number: Int)  end` -/
def Command : DataDecl :=
  choice "Command" [("Add", [("text", tText)]), ("Show", []), ("Done", [("number", tInt)]),
    ("Remove", [("number", tInt)])]

/-- `enum TodoError  Usage  BadLine(number: Int)  NoSuchItem(number: Int)  Storage(FileError)  end` -/
def TodoError : DataDecl :=
  choice "TodoError" [("Usage", []), ("BadLine", [("number", tInt)]), ("NoSuchItem", [("number", tInt)]),
    ("Storage", [("0", tFileError)])]

def items : Ty := tList Item.ty
def itemsRes : Ty := tRes items TodoError.ty
def noSuchItem (n : Expr) : Expr := mkC TodoError "NoSuchItem" [n]

/-- `file = "todo.txt"` -/
def fileC : FunDecl := const "file" tText (str "todo.txt")

/-- ```
fn to_text(problem: TodoError) : Text
  case problem
    Usage: "usage: todo add TEXT | list | done N | remove N"
    BadLine(number: n): "line #{n} of #{file} isn't an item"
    NoSuchItem(number: n): "there is no item #{n}"
    Storage(failure): "#{failure}"
  end
end
``` -/
def toText : FunDecl :=
  fn "TodoError.to_text" [("problem", TodoError.ty)] tText <|
    .case_ (v "problem")
      [("Usage", [], str "usage: todo add TEXT | list | done N | remove N"),
       ("BadLine", ["n"], cat [str "line ", intText (v "n"), str " of ", call "file" [], str " isn't an item"]),
       ("NoSuchItem", ["n"], cat [str "there is no item ", intText (v "n")]),
       ("Storage", ["failure"], fileErrorTextL (v "failure"))]

/-- ```
fn parse_command(args: List(Text)) : Option(Command)
  case args
    ["add", ..words] if not words.empty?: Some(Add(text: words.join(" ")))
    ["list"]: Some(Show)
    ["done", n]: Some(Done(number: try n.to_int))
    ["remove", n]: Some(Remove(number: try n.to_int))
    _: None
  end
end
```
A decision tree: the first element is compared with each literal in turn;
every way of failing to match goes to the last arm, the join point `%none`. -/
def parseCommand : FunDecl :=
  fn "parse_command" [("args", tList tText)] (tOpt Command.ty) <|
    .join "%none" [] (none_ Command.ty) <|
      caseOpt (lop tText .uncons [v "args"]) "%p"
        (lets [("%first", fst (v "%p")), ("%rest", snd (v "%p"))] <|
          ite (teq (v "%first") (str "add"))
            (lets [("words", v "%rest")] <|
              ite (not_ (lop tText .isEmpty [v "words"]))
                (some_ Command.ty (mkC Command "Add" [top .join [v "words", str " "]]))
                (.jump "%none" [])) <|
          ite (teq (v "%first") (str "list"))
            (matchList tText (v "%rest") [] none (some_ Command.ty (mkC Command "Show" [])) (.jump "%none" [])) <|
          ite (teq (v "%first") (str "done"))
            (matchList tText (v "%rest") ["n"] none
              (tryOpt Command.ty (top .toInt [v "n"]) "%n" (some_ Command.ty (mkC Command "Done" [v "%n"])))
              (.jump "%none" [])) <|
          ite (teq (v "%first") (str "remove"))
            (matchList tText (v "%rest") ["n"] none
              (tryOpt Command.ty (top .toInt [v "n"]) "%n" (some_ Command.ty (mkC Command "Remove" [v "%n"])))
              (.jump "%none" []))
            (.jump "%none" []))
        (.jump "%none" [])

/-- ```
fn parse_items(text: Text) : Result(List(Item), TodoError)
  var items = []
  for (index, line) in text.lines.enumerate
    item = case line.drop_prefix("[x] ")
      Some(rest): Item(text: rest, done?: true)
      None:
        rest = try line.drop_prefix("[ ] ").or_fail(BadLine(number: index + 1))
        Item(text: rest, done?: false)
    end
    items = items.append(item)
  end
  Ok(items)
end
``` -/
def parseItems : FunDecl :=
  fn "parse_items" [("text", tText)] itemsRes <|
    lets [("items", nil Item.ty)] <|
      forList "l" (tTup [tInt, tText]) "%il" (lop tText .enumerate [top .lines [v "text"]])
        [("items", items)] [v "items"]
        (lets [("index", fst (v "%il")), ("line", snd (v "%il"))] <|
          .join "%item" [("item", Item.ty)] (nextRound "l" [lop Item.ty .append [v "items", v "item"]]) <|
            caseOpt (top .dropPrefix [v "line", str "[x] "]) "rest"
              (.jump "%item" [mk Item [v "rest", tt]])
              (tryRes items TodoError.ty
                (orFail tText TodoError.ty (top .dropPrefix [v "line", str "[ ] "])
                  (mkC TodoError "BadLine" [iadd (v "index") (int 1)])) "rest" <|
                .jump "%item" [mk Item [v "rest", ff]]))
        (ok_ items TodoError.ty (v "items"))

/-- ```
fn format(items: List(Item)) : Text
  items.map(fn(item) "#{box(item)} #{item.text}\n" end).join("")
end
``` -/
def format : FunDecl :=
  fn "format" [("items", items)] tText <|
    top .join [mapL Item.ty tText (v "items")
      (lam [("item", Item.ty)] (cat [call "box" [v "item"], str " ", get Item "text" (v "item"), str "\n"])),
      str ""]

/-- `fn box(item: Item) : Text  if item.done?: "[x]" else: "[ ]"  end` -/
def box : FunDecl :=
  fn "box" [("item", Item.ty)] tText <| ite (get Item "done?" (v "item")) (str "[x]") (str "[ ]")

/-- ```
fn apply(items: List(Item), command: Command) : Result(List(Item), TodoError)
  case command
    Add(text: text): Ok(items.append(Item(text: text, done?: false)))
    Show: Ok(items)
    Done(number: n):
      return Error(NoSuchItem(number: n)) if n < 1
      var item = try items.get(n - 1).or_fail(NoSuchItem(number: n))
      item.done? = true
      items.set(n - 1, item).or_fail(NoSuchItem(number: n))
    Remove(number: n):
      return Error(NoSuchItem(number: n)) if n < 1
      items.remove_at(n - 1).or_fail(NoSuchItem(number: n))
  end
end
``` -/
def apply : FunDecl :=
  fn "apply" [("items", items), ("command", Command.ty)] itemsRes <|
    .case_ (v "command")
      [("Add", ["text"], ok_ items TodoError.ty (lop Item.ty .append [v "items", mk Item [v "text", ff]])),
       ("Show", [], ok_ items TodoError.ty (v "items")),
       ("Done", ["n"],
         ite (ilt (v "n") (int 1)) (err_ items TodoError.ty (noSuchItem (v "n"))) <|
         tryRes items TodoError.ty
           (orFail Item.ty TodoError.ty (lop Item.ty .get [v "items", isub (v "n") (int 1)]) (noSuchItem (v "n")))
           "item" <|
         lets [("item", setField Item "done?" (v "item") tt)] <|
           orFail items TodoError.ty (lop Item.ty .set [v "items", isub (v "n") (int 1), v "item"])
             (noSuchItem (v "n"))),
       ("Remove", ["n"],
         ite (ilt (v "n") (int 1)) (err_ items TodoError.ty (noSuchItem (v "n"))) <|
           orFail items TodoError.ty (lop Item.ty .removeAt [v "items", isub (v "n") (int 1)])
             (noSuchItem (v "n")))]

/-- ```
fn main(args: Args, console: Console, files: Files) : Result(Unit, TodoError)
  command = try parse_command(args.list).or_fail(Usage)
  saved = case files.read_text(file)
    Ok(text): text
    Error(NotFound(path: _)): ""
    Error(failure): return Error(Storage(failure))
  end
  items = try apply(try parse_items(saved), command)
  if command == Show
    for (index, item) in items.enumerate
      console.print("#{index + 1}. #{box(item)} #{item.text}")
    end
  else
    try files.write_text(file, format(items))
  end
  Ok(Unit)
end
```
`try files.write_text(...)` wraps its `FileError` in `Storage`, the one case of
`TodoError` that holds one (reference 8.3). -/
def mainFn : FunDecl :=
  fn "main" [("args", tCap .args), ("console", tCap .console), ("files", tCap .files)]
    (tRes tUnit TodoError.ty) <|
    tryRes tUnit TodoError.ty
      (orFail Command.ty TodoError.ty (call "parse_command" [argsList (v "args")]) (mkC TodoError "Usage" []))
      "command" <|
    .join "%saved" [("saved", tText)]
      (tryRes tUnit TodoError.ty (call "parse_items" [v "saved"]) "%parsed" <|
       tryRes tUnit TodoError.ty (call "apply" [v "%parsed", v "command"]) "items" <|
       .join "%after" [] (ok_ tUnit TodoError.ty unitE) <|
         ite (eq Command.ty (v "command") (mkC Command "Show" []))
           (forList "show" (tTup [tInt, Item.ty]) "%ii" (lop Item.ty .enumerate [v "items"]) [] []
             (lets [("index", fst (v "%ii")), ("item", snd (v "%ii"))] <|
               seq (print (v "console") (cat [intText (iadd (v "index") (int 1)), str ". ",
                     call "box" [v "item"], str " ", get Item "text" (v "item")])) <|
                 nextRound "show" [])
             (.jump "%after" []))
           (tryWrap tUnit TodoError.ty (io .writeText [v "files", call "file" [], call "format" [v "items"]])
             "_" (fun e => mkC TodoError "Storage" [e]) <|
             .jump "%after" []))
      (caseRes (io (.readText false) [v "files", call "file" []])
        "text" (.jump "%saved" [v "text"])
        "failure"
          (.case_ (v "failure")
            [("NotFound", ["_"], .jump "%saved" [str ""]),
             ("NotAllowed", ["_"], err_ tUnit TodoError.ty (mkC TodoError "Storage" [v "failure"])),
             ("NotText", ["_"], err_ tUnit TodoError.ty (mkC TodoError "Storage" [v "failure"])),
             ("Failed", ["_", "_"], err_ tUnit TodoError.ty (mkC TodoError "Storage" [v "failure"]))]))

/-- `milk = Item(text: "milk", done?: false)` -/
def milk : FunDecl := const "milk" Item.ty (mk Item [str "milk", ff])

/-- `milk_done = Item(text: "milk", done?: true)` -/
def milkDone : FunDecl := const "milk_done" Item.ty (mk Item [str "milk", tt])

/-- apply: "marking an item done keeps every item"
`command is Done(number: _) and result is Ok(after) implies after.size == items.size` -/
def applyKeepsItems : FunDecl :=
  fn "apply.promise.1" [("items", items), ("command", Command.ty), ("result", itemsRes)] tBool <|
    .case_ (v "command")
      [("Add", ["_"], tt), ("Show", [], tt),
       ("Done", ["_"],
         caseRes (v "result") "after" (ieq (lop Item.ty .size [v "after"]) (lop Item.ty .size [v "items"]))
           "_" tt),
       ("Remove", ["_"], tt)]

def program : Program :=
  { data := [Item, Command, TodoError]
    funs := [fileC, toText, parseCommand, parseItems, format, box, apply, mainFn, milk, milkDone,
             applyKeepsItems, Lib.map Item.ty tText, Lib.fileErrorText] }

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

def cmd (args : List String) : Expr := call "parse_command" [strs args]
def someCmd (e : Expr) : Expr := some_ Command.ty e
def item (text : String) (done : Bool) : Expr := mk Item [str text, bool done]
def okItems (es : List Expr) : Expr := ok_ items TodoError.ty (listOf Item.ty es)

-- parse_command(["add", "buy", "milk"]) == Some(Add(text: "buy milk"))
#guard passes program (eq (tOpt Command.ty) (cmd ["add", "buy", "milk"]) (someCmd (mkC Command "Add" [str "buy milk"])))
-- parse_command(["list"]) == Some(Show)
#guard passes program (eq (tOpt Command.ty) (cmd ["list"]) (someCmd (mkC Command "Show" [])))
-- parse_command(["done", "2"]) == Some(Done(number: 2))
#guard passes program (eq (tOpt Command.ty) (cmd ["done", "2"]) (someCmd (mkC Command "Done" [int 2])))
-- parse_command(["done", "two"]) == None
#guard passes program (eq (tOpt Command.ty) (cmd ["done", "two"]) (none_ Command.ty))
-- parse_command(["add"]) == None
#guard passes program (eq (tOpt Command.ty) (cmd ["add"]) (none_ Command.ty))
-- parse_items("[ ] milk\n[x] eggs\n") == Ok([Item(text: "milk", done?: false), Item(text: "eggs", done?: true)])
#guard passes program
  (eq itemsRes (call "parse_items" [str "[ ] milk\n[x] eggs\n"]) (okItems [item "milk" false, item "eggs" true]))
-- parse_items("") == Ok([])
#guard passes program (eq itemsRes (call "parse_items" [str ""]) (okItems []))
-- parse_items("milk\n") == Error(BadLine(number: 1))
#guard passes program
  (eq itemsRes (call "parse_items" [str "milk\n"]) (err_ items TodoError.ty (mkC TodoError "BadLine" [int 1])))
-- format([Item(text: "milk", done?: false), Item(text: "eggs", done?: true)]) == "[ ] milk\n[x] eggs\n"
#guard passes program
  (teq (call "format" [listOf Item.ty [item "milk" false, item "eggs" true]]) (str "[ ] milk\n[x] eggs\n"))
-- apply([milk], Done(number: 1)) == Ok([milk_done])
#guard passes program
  (eq itemsRes (call "apply" [listOf Item.ty [call "milk" []], mkC Command "Done" [int 1]])
    (okItems [call "milk_done" []]))
-- apply([milk], Remove(number: 0)) == Error(NoSuchItem(number: 0))
#guard passes program
  (eq itemsRes (call "apply" [listOf Item.ty [call "milk" []], mkC Command "Remove" [int 0]])
    (err_ items TodoError.ty (noSuchItem (int 0))))
-- apply([milk], Remove(number: 2)) == Error(NoSuchItem(number: 2))
#guard passes program
  (eq itemsRes (call "apply" [listOf Item.ty [call "milk" []], mkC Command "Remove" [int 2]])
    (err_ items TodoError.ty (noSuchItem (int 2))))

/-! ### The promise's anchors -/

-- allows items: [milk], command: Done(number: 1), result: Ok([milk_done])
#guard passes program
  (call "apply.promise.1" [listOf Item.ty [call "milk" []], mkC Command "Done" [int 1], okItems [call "milk_done" []]])
-- forbids items: [milk], command: Done(number: 1), result: Ok([])
#guard passes program
  (not_ (call "apply.promise.1" [listOf Item.ty [call "milk" []], mkC Command "Done" [int 1], okItems []]))

/-! ### Test "items added and marked done are saved, and listed by number"

```
files = Files.in_memory({})
console = Console.fake()
assert main(Args.of(["add", "buy", "milk"]), console, files) == Ok(Unit)
assert main(Args.of(["add", "call", "Ana"]), console, files) == Ok(Unit)
assert main(Args.of(["done", "1"]), console, files) == Ok(Unit)
assert files.read_text(file) == Ok("[x] buy milk\n[ ] call Ana\n")
assert main(Args.of(["list"]), console, files) == Ok(Unit)
assert console.printed == ["1. [x] buy milk", "2. [ ] call Ana"]
```
One world holds the shared files and console; each `Args.of(...)` is a new
fake, set before the step that uses it. -/
def runMain : Expr := call "main" [v "args", v "console", v "files"]
def okUnit : Expr := ok_ tUnit TodoError.ty unitE
def caps : List (String × Cap) := [("args", .args), ("console", .console), ("files", .files)]

#guard scenario program caps {}
  [(withArgs ["add", "buy", "milk"], eq (tRes tUnit TodoError.ty) runMain okUnit),
   (withArgs ["add", "call", "Ana"], eq (tRes tUnit TodoError.ty) runMain okUnit),
   (withArgs ["done", "1"], eq (tRes tUnit TodoError.ty) runMain okUnit),
   (same, eq (tRes tText tFileError) (io (.readText false) [v "files", call "file" []])
            (ok_ tText tFileError (str "[x] buy milk\n[ ] call Ana\n"))),
   (withArgs ["list"], eq (tRes tUnit TodoError.ty) runMain okUnit),
   (same, eq (tList tText) (io .printed [v "console"]) (strs ["1. [x] buy milk", "2. [ ] call Ana"]))]

/-! ### Test "an item that isn't there is reported"

```
files = Files.in_memory({file: "[ ] milk\n"})
assert main(Args.of(["remove", "3"]), Console.fake(), files) == Error(NoSuchItem(number: 3))
``` -/
#guard passes program
  (eq (tRes tUnit TodoError.ty) runMain (err_ tUnit TodoError.ty (noSuchItem (int 3))))
  { args := ["remove", "3"], files := [("todo.txt", "[ ] milk\n")] } caps

/-! ### Never wrong -/

/-- Running `main` never goes wrong, whatever the arguments and the file. -/
theorem main_never_wrong (n : Nat) (w : World) (msg : String) :
    eval n program (handles caps) w runMain ≠ .wrong msg :=
  never_wrong_with_handles (τ := tRes tUnit TodoError.ty) checked (by decide +kernel) n w msg

end MoCore.Examples.Todo

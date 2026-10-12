/-
  examples/expenses.mo in Core.

  Exercises: text to numbers, exact money, maps with `update`, a stable sort
  (`sort_by_desc`), list patterns as decision trees, `continue`, a `case` used
  as a value with an early `return` in one arm (a join point), and
  `to_text` customized for a choice type.
-/
import MoCore.Testing

namespace MoCore.Examples.Expenses

open MoCore DSL Lib

/-- `struct Expense  category: Text  amount: Decimal  end` -/
def Expense : DataDecl := record "Expense" [("category", tText), ("amount", tDec)]

/-- `enum LineError  WrongFieldCount(found: Int)  NoCategory  BadAmount(text: Text)  end` -/
def LineError : DataDecl :=
  choice "LineError" [("WrongFieldCount", [("found", tInt)]), ("NoCategory", []),
    ("BadAmount", [("text", tText)])]

def resTy : Ty := tRes Expense.ty LineError.ty
def entryTy : Ty := tTup [tText, tDec]

/-- ```
fn to_text(problem: LineError) : Text
  case problem
    WrongFieldCount(found: n): "expected 3 fields, found #{n}"
    NoCategory: "the category is empty"
    BadAmount(text: t): "the amount \"#{t}\" is not a number"
  end
end
``` -/
def toText : FunDecl :=
  fn "LineError.to_text" [("problem", LineError.ty)] tText <|
    .case_ (v "problem")
      [("WrongFieldCount", ["n"], cat [str "expected 3 fields, found ", intText (v "n")]),
       ("NoCategory", [], str "the category is empty"),
       ("BadAmount", ["t"], cat [str "the amount \"", v "t", str "\" is not a number"])]

/-- ```
fn parse_line(line: Text) : Result(Expense, LineError)
  case line.split(",").map(fn(field) field.trim end)
    [_, category, amount]:
      return Error(NoCategory) if category.empty?
      value = try amount.to_decimal.or_fail(BadAmount(text: amount))
      Ok(Expense(category: category, amount: value))
    fields: Error(WrongFieldCount(found: fields.size))
  end
end
```
The list pattern is a decision tree; its fallback arm is a join point. -/
def parseLine : FunDecl :=
  fn "parse_line" [("line", tText)] resTy <|
    lets [("fields", mapL tText tText (top .split [v "line", str ","])
                       (lam [("field", tText)] (top .trim [v "field"])))] <|
      .join "%other" []
        (err_ Expense.ty LineError.ty (mkC LineError "WrongFieldCount" [lop tText .size [v "fields"]])) <|
        matchList tText (v "fields") ["_", "category", "amount"] none
          (ite (top .isEmpty [v "category"])
            (err_ Expense.ty LineError.ty (mkC LineError "NoCategory" [])) <|
            tryRes Expense.ty LineError.ty
              (orFail tDec LineError.ty (top .toDecimal [v "amount"]) (mkC LineError "BadAmount" [v "amount"]))
              "value" <|
              ok_ Expense.ty LineError.ty (mk Expense [v "category", v "value"]))
          (.jump "%other" [])

/-- ```
fn totals(expenses: List(Expense)) : List((Text, Decimal))
  var sums = {}
  for expense in expenses
    sums = sums.update(expense.category, 0, fn(sum) sum + expense.amount end)
  end
  sums.entries.sort_by_desc(fn(entry) entry.1 end)
end
``` -/
def totals : FunDecl :=
  fn "totals" [("expenses", tList Expense.ty)] (tList entryTy) <|
    lets [("sums", emptyMap tText tDec)] <|
      forList "e" Expense.ty "expense" (v "expenses") [("sums", tMap tText tDec)] [v "sums"]
        (nextRound "e" [updateL tText tDec (v "sums") (get Expense "category" (v "expense")) (dec "0")
          (lam [("sum", tDec)] (dadd (v "sum") (get Expense "amount" (v "expense"))))])
        (sortByDescL entryTy tDec (mop tText tDec .entries [v "sums"])
          (lam [("entry", entryTy)] (snd (v "entry"))))

/-- ```
fn layout(rows: List((Text, Decimal))) : List(Text)
  names = rows.map(fn(row) row.0 end)
  amounts = rows.map(fn(row) row.1.to_fixed(2) end)
  name_width = names.map(fn(name) name.char_count end).max or 0
  amount_width = amounts.map(fn(amount) amount.char_count end).max or 0
  var lines = []
  for (name, amount) in names.zip(amounts)
    lines = lines.append("#{name.pad_right(name_width)}  #{amount.pad_left(amount_width)}")
  end
  lines
end
``` -/
def layout : FunDecl :=
  fn "layout" [("rows", tList entryTy)] (tList tText) <|
    lets [("names", mapL entryTy tText (v "rows") (lam [("row", entryTy)] (fst (v "row")))),
          ("amounts", mapL entryTy tText (v "rows")
            (lam [("row", entryTy)] (dop .toFixed [snd (v "row"), int 2]))),
          ("name_width", orElse (maxL tInt (mapL tText tInt (v "names")
            (lam [("name", tText)] (top .charCount [v "name"])))) (int 0)),
          ("amount_width", orElse (maxL tInt (mapL tText tInt (v "amounts")
            (lam [("amount", tText)] (top .charCount [v "amount"])))) (int 0)),
          ("lines", nil tText)] <|
      forList "z" (tTup [tText, tText]) "%pair" (lop tText (.zip tText) [v "names", v "amounts"])
        [("lines", tList tText)] [v "lines"]
        (lets [("name", fst (v "%pair")), ("amount", snd (v "%pair"))] <|
          nextRound "z" [lop tText .append [v "lines",
            cat [top .padRight [v "name", v "name_width"], str "  ",
                 top .padLeft [v "amount", v "amount_width"]]]])
        (v "lines")

/-- ```
fn main(args: Args, console: Console, files: ReadFiles) : Int
  case args.list
    [path]: report(path, console, files)
    _:
      console.error("usage: expenses FILE.csv")
      2
  end
end
``` -/
def mainFn : FunDecl :=
  fn "main" [("args", tCap .args), ("console", tCap .console), ("files", tCap .readFiles)] tInt <|
    lets [("%list", argsList (v "args"))] <|
      .join "%usage" [] (seq (printError (v "console") (str "usage: expenses FILE.csv")) (int 2)) <|
        matchList tText (v "%list") ["path"] none
          (call "report" [v "path", v "console", v "files"])
          (.jump "%usage" [])

/-- ```
fn report(path: Text, console: Console, files: ReadFiles) : Int
  text = case files.read_text(path)
    Ok(contents): contents
    Error(failure):
      console.error("expenses: #{failure}")
      return 2
  end
  var expenses = []
  var malformed? = false
  for (index, line) in text.lines.enumerate
    continue if index == 0 or line.trim.empty?
    case parse_line(line)
      Ok(expense): expenses = expenses.append(expense)
      Error(problem):
        console.error("line #{index + 1}: #{problem}")
        malformed? = true
    end
  end
  for line in layout(totals(expenses))
    console.print(line)
  end
  if malformed?: 1 else: 0
end
```
`text = case ...` binds the rest of the body as the join point `%text`; the
arm that returns gives the function's result directly. -/
def report : FunDecl :=
  fn "report" [("path", tText), ("console", tCap .console), ("files", tCap .readFiles)] tInt <|
    .join "%text" [("text", tText)]
      (lets [("expenses", nil Expense.ty), ("malformed?", ff)] <|
        forList "lines" (tTup [tInt, tText]) "%il" (lop tText .enumerate [top .lines [v "text"]])
          [("expenses", tList Expense.ty), ("malformed?", tBool)] [v "expenses", v "malformed?"]
          (lets [("index", fst (v "%il")), ("line", snd (v "%il"))] <|
            ite (or_ (ieq (v "index") (int 0)) (top .isEmpty [top .trim [v "line"]]))
              (nextRound "lines" [v "expenses", v "malformed?"]) <|
              caseRes (call "parse_line" [v "line"])
                "expense"
                  (nextRound "lines" [lop Expense.ty .append [v "expenses", v "expense"], v "malformed?"])
                "problem"
                  (seq (printError (v "console")
                      (cat [str "line ", intText (iadd (v "index") (int 1)), str ": ",
                            call "LineError.to_text" [v "problem"]])) <|
                    nextRound "lines" [v "expenses", tt]))
          (forList "out" tText "line" (call "layout" [call "totals" [v "expenses"]]) [] []
            (seq (print (v "console") (v "line")) (nextRound "out" []))
            (ite (v "malformed?") (int 1) (int 0))))
      (caseRes (io (.readText true) [v "files", v "path"])
        "contents" (.jump "%text" [v "contents"])
        "failure"
          (seq (printError (v "console") (cat [str "expenses: ", fileErrorTextL (v "failure")])) (int 2)))

/-- `fn food(amount: Decimal) : Expense  Expense(category: "food", amount: amount)  end` -/
def food : FunDecl := fn "food" [("amount", tDec)] Expense.ty <| mk Expense [str "food", v "amount"]

/-- `fn rent(amount: Decimal) : Expense  Expense(category: "rent", amount: amount)  end` -/
def rent : FunDecl := fn "rent" [("amount", tDec)] Expense.ty <| mk Expense [str "rent", v "amount"]

def program : Program :=
  wholeProgram "Expenses" "main" [Expense, LineError] [] <|
    [toText, parseLine, totals, layout, mainFn, report, food, rent,
             Lib.map tText tText, Lib.map entryTy tText, Lib.map tText tInt, Lib.max tInt,
             Lib.update tText tDec, Lib.fileErrorText] ++ Lib.sortByDesc entryTy tDec

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

def okE (e : Expr) : Expr := ok_ Expense.ty LineError.ty e
def errE (e : Expr) : Expr := err_ Expense.ty LineError.ty e

-- parse_line("2026-10-01,food,12.50") == Ok(Expense(category: "food", amount: 12.50))
#guard passes program
  (eq resTy (call "parse_line" [str "2026-10-01,food,12.50"]) (okE (mk Expense [str "food", dec "12.50"])))
-- parse_line("2026-10-01, rent , 900") == Ok(Expense(category: "rent", amount: 900))
#guard passes program
  (eq resTy (call "parse_line" [str "2026-10-01, rent , 900"]) (okE (mk Expense [str "rent", dec "900"])))
-- parse_line("2026-10-01,food") == Error(WrongFieldCount(found: 2))
#guard passes program
  (eq resTy (call "parse_line" [str "2026-10-01,food"]) (errE (mkC LineError "WrongFieldCount" [int 2])))
-- parse_line("2026-10-01,,3") == Error(NoCategory)
#guard passes program
  (eq resTy (call "parse_line" [str "2026-10-01,,3"]) (errE (mkC LineError "NoCategory" [])))
-- parse_line("2026-10-01,food,twelve") == Error(BadAmount(text: "twelve"))
#guard passes program
  (eq resTy (call "parse_line" [str "2026-10-01,food,twelve"])
    (errE (mkC LineError "BadAmount" [str "twelve"])))
-- totals([food(1.50), rent(4), food(3)]) == [("food", 4.50), ("rent", 4)]
#guard passes program
  (eq (tList entryTy)
    (call "totals" [listOf Expense.ty [call "food" [dec "1.50"], call "rent" [dec "4"], call "food" [dec "3"]]])
    (listOf entryTy [tup [str "food", dec "4.50"], tup [str "rent", dec "4"]]))
-- totals([]) == []
#guard passes program (eq (tList entryTy) (call "totals" [nil Expense.ty]) (nil entryTy))
-- layout([("rent", 900), ("food", 12.5)]) == ["rent  900.00", "food   12.50"]
#guard passes program
  (eq (tList tText)
    (call "layout" [listOf entryTy [tup [str "rent", dec "900"], tup [str "food", dec "12.5"]]])
    (strs ["rent  900.00", "food   12.50"]))

/-! ### Test "a malformed line is reported by number, and the rest still totalled"

```
csv = "date,category,amount\n2026-10-01,food,12.50\n2026-10-02,food,x\n2026-10-03,rent,900\n"
files = Files.in_memory({"october.csv": csv}).read_only
console = Console.fake()
assert main(Args.of(["october.csv"]), console, files) == 1
assert console.printed == ["rent  900.00", "food   12.50"]
assert console.printed_errors == ["line 3: the amount \"x\" is not a number"]
``` -/
#guard passes program
  (and_ (ieq (call "main" [v "args", v "console", v "files"]) (int 1)) <|
    and_ (eq (tList tText) (io .printed [v "console"]) (strs ["rent  900.00", "food   12.50"])) <|
      eq (tList tText) (io .printedErrors [v "console"])
        (strs ["line 3: the amount \"x\" is not a number"]))
  { args := ["october.csv"],
    files := [("october.csv",
      "date,category,amount\n2026-10-01,food,12.50\n2026-10-02,food,x\n2026-10-03,rent,900\n")] }
  [("args", .args), ("console", .console), ("files", .readFiles)]

/-! ### Never wrong -/

/-- Running `main` never goes wrong, whatever the arguments and files. -/
theorem main_never_wrong (n : Nat) (w : World) (msg : String) :
    evalProgram n program (handles [("args", .args), ("console", .console), ("files", .readFiles)]) w
      (call "main" [v "args", v "console", v "files"]) ≠ .wrong msg :=
  never_wrong_with_handles (τ := tInt) checked (by decide +kernel) n w msg

end MoCore.Examples.Expenses

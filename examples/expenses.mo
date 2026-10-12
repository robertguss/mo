## Totals a CSV file of expenses by category, largest first.
##
##   expenses FILE.csv
##
## The first line is a header. Every other line is date,category,amount, with
## the amount in a currency's main unit, such as 12.50. A malformed line is
## reported with its line number and skipped, and the exit status is then 1.
module Expenses

struct Expense
  category: Text
  amount: Decimal
end

enum LineError
  WrongFieldCount(found: Int)
  NoCategory
  BadAmount(text: Text)
end

fn to_text(problem: LineError) : Text
  case problem
    WrongFieldCount(found: n): "expected 3 fields, found #{n}"
    NoCategory: "the category is empty"
    BadAmount(text: t): "the amount \"#{t}\" is not a number"
  end
end

fn parse_line(line: Text) : Result(Expense, LineError)
  examples
    parse_line("2026-10-01,food,12.50") == Ok(Expense(category: "food", amount: 12.50))
    parse_line("2026-10-01, rent , 900") == Ok(Expense(category: "rent", amount: 900))
    parse_line("2026-10-01,food") == Error(WrongFieldCount(found: 2))
    parse_line("2026-10-01,,3") == Error(NoCategory)
    parse_line("2026-10-01,food,twelve") == Error(BadAmount(text: "twelve"))
  end

  case line.split(",").map(fn(field) field.trim end)
    [_, category, amount]:
      return Error(NoCategory) if category.empty?
      value = try amount.to_decimal.or_fail(BadAmount(text: amount))
      Ok(Expense(category: category, amount: value))
    fields: Error(WrongFieldCount(found: fields.size))
  end
end

## Each category's total, largest first. Categories with equal totals keep the
## order in which they first appeared.
fn totals(expenses: List(Expense)) : List((Text, Decimal))
  examples
    totals([food(1.50), rent(4), food(3)]) == [("food", 4.50), ("rent", 4)]
    totals([]) == []
  end

  var sums = {}
  for expense in expenses
    sums = sums.update(expense.category, 0, fn(sum) sum + expense.amount end)
  end
  sums.entries.sort_by_desc(fn(entry) entry.1 end)
end

## The totals as lines, with the categories and the amounts lined up.
fn layout(rows: List((Text, Decimal))) : List(Text)
  examples
    layout([("rent", 900), ("food", 12.5)]) == ["rent  900.00", "food   12.50"]
  end

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

fn main(args: Args, console: Console, files: ReadFiles) : Int
  case args.list
    [path]: report(path, console, files)
    _:
      console.error("usage: expenses FILE.csv")
      2
  end
end

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

fn food(amount: Decimal) : Expense
  Expense(category: "food", amount: amount)
end

fn rent(amount: Decimal) : Expense
  Expense(category: "rent", amount: amount)
end

test "a malformed line is reported by number, and the rest still totalled"
  csv = "date,category,amount\n2026-10-01,food,12.50\n2026-10-02,food,x\n2026-10-03,rent,900\n"
  files = Files.in_memory({"october.csv": csv}).read_only
  console = Console.fake()
  assert main(Args.of(["october.csv"]), console, files) == 1
  assert console.printed == ["rent  900.00", "food   12.50"]
  assert console.printed_errors == ["line 3: the amount \"x\" is not a number"]
end

## Totals a CSV file of expenses by category, largest first.
##
## Usage: expenses FILE
##
## The first line of FILE is a header, and every other line is
## date,category,amount, such as 2026-10-01,food,12.50. Spaces around a field
## don't count, blank lines are skipped, and no field can hold a comma. The
## date may take any form but can't be empty; the amount is a number with at
## most two decimal places, negative for a refund.
##
## Prints each category and its total in two columns, the largest total first
## and equal totals in order of category. A malformed line is reported on
## standard error with its line number and skipped, and the exit status is
## then 1. Without exactly one FILE, or when FILE can't be read, the program
## says why on standard error and exits with status 2.
module Expenses

## What a well-formed line records, as far as the totals go.
struct Expense
  category: Text
  amount: Decimal
end

## A category, and the sum of its expenses.
struct Total
  category: Text
  amount: Decimal
end

## Why a line isn't date,category,amount.
enum LineError
  FieldCount(found: Int)
  Missing(field: Text)
  NotANumber(text: Text)
  TooManyDecimals(text: Text)
end

## A line's problem, in words.
fn to_text(problem: LineError) : Text
  case problem
    FieldCount(found: count): "expected 3 fields (date,category,amount) but found #{count}"
    Missing(field: field): "the #{field} is missing"
    NotANumber(text: amount): "the amount \"#{amount}\" is not a number"
    TooManyDecimals(text: amount): "the amount \"#{amount}\" has more than two decimal places"
  end
end

## Prints the totals of the file named on the command line, and reports its
## malformed lines.
fn main(args: Args, console: Console, files: ReadFiles) : Int
  path = case args.list
    [name]: name
    _:
      console.error("usage: expenses FILE")
      return 2
  end
  text = case files.read_text(path)
    Ok(contents): contents
    Error(failure):
      console.error("expenses: #{failure}")
      return 2
  end

  var expenses = []
  var malformed? = false
  for (index, line) in text.lines.enumerate.drop(1)  # line 1 is the header
    continue if line.trim.empty?
    case parse_line(line)
      Ok(expense): expenses = expenses.append(expense)
      Error(problem):
        console.error("line #{index + 1}: #{problem}")
        malformed? = true
    end
  end
  for row in format_totals(totals_by_category(expenses))
    console.print(row)
  end
  if malformed?: 1 else: 0
end

## Reads a line after the header, date,category,amount, into the expense it
## records.
fn parse_line(line: Text) : Result(Expense, LineError)
  examples
    parse_line("2026-10-01,food,12.50") == Ok(Expense(category: "food", amount: 12.50))
    parse_line(" 2026-10-02 , rent , 800 ") == Ok(Expense(category: "rent", amount: 800))
    parse_line("2026-10-03,food,-4.5") == Ok(Expense(category: "food", amount: -4.5))
    parse_line("2026-10-04,food") == Error(FieldCount(found: 2))
    parse_line("2026-10-05,food,1,250.00") == Error(FieldCount(found: 4))
    parse_line("2026-10-06,,3.00") == Error(Missing(field: "category"))
    parse_line("2026-10-07,food,ten") == Error(NotANumber(text: "ten"))
    parse_line("2026-10-08,food,1.005") == Error(TooManyDecimals(text: "1.005"))
  end

  fields = line.split(",").map(fn(field) field.trim end)
  if fields is [date, category, amount]
    return Error(Missing(field: "date")) if date.empty?
    return Error(Missing(field: "category")) if category.empty?
    return Error(Missing(field: "amount")) if amount.empty?
    value = try amount.to_decimal.or_fail(NotANumber(text: amount))
    return Error(TooManyDecimals(text: amount)) if value.round(2) != value
    Ok(Expense(category: category, amount: value))
  else
    Error(FieldCount(found: fields.size))
  end
end

## Each category with the sum of its amounts: the largest sum first, and equal
## sums in order of category.
fn totals_by_category(expenses: List(Expense)) : List(Total)
  examples
    totals_by_category([]) == []
    totals_by_category([
      Expense(category: "food", amount: 4.50),
      Expense(category: "rent", amount: 9),
      Expense(category: "food", amount: 5),
      Expense(category: "fun", amount: 9)
    ]) == [
      Total(category: "food", amount: 9.50),
      Total(category: "fun", amount: 9),
      Total(category: "rent", amount: 9)
    ]
  end

  groups = expenses.group_by(fn(expense) expense.category end)
  var totals = []
  for (category, group) in groups
    amount = group.map(fn(expense) expense.amount end).sum
    totals = totals.append(Total(category: category, amount: amount))
  end
  totals
    .sort_by(fn(total) total.category end)
    .sort_by_desc(fn(total) total.amount end)
end

## The lines that show the totals: each category, then its total to the cent,
## in two columns.
fn format_totals(totals: List(Total)) : List(Text)
  examples
    format_totals([]) == []
    format_totals([
      Total(category: "rent", amount: 800),
      Total(category: "food", amount: 19.75)
    ]) == ["rent  800.00", "food   19.75"]
  end

  name_width = totals.map(fn(total) total.category.char_count end).max or 0
  amount_width = totals.map(fn(total) total.amount.to_fixed(2).char_count end).max or 0
  totals.map(fn(total)
    name = total.category.pad_right(name_width)
    amount = total.amount.to_fixed(2).pad_left(amount_width)
    "#{name}  #{amount}"
  end)
end

test "expenses are added up by category, largest first"
  csv = """
    date,category,amount
    2026-10-01,food,12.50
    2026-10-02,rent,800.00
    2026-10-03,food,7.25
    """
  console = Console.fake()
  files = Files.in_memory({"october.csv": csv}).read_only
  assert main(Args.of(["october.csv"]), console, files) == 0
  assert console.printed == ["rent  800.00", "food   19.75"]
  assert console.printed_errors == []
end

test "a malformed line is reported with its number and skipped, and the status is 1"
  csv = """
    date,category,amount
    2026-10-01,food,12.50
    2026-10-02,food
    2026-10-03,fun,ten
    2026-10-04,fun,3
    """
  console = Console.fake()
  files = Files.in_memory({"october.csv": csv}).read_only
  assert main(Args.of(["october.csv"]), console, files) == 1
  assert console.printed == ["food  12.50", "fun    3.00"]
  assert console.printed_errors == [
    "line 3: expected 3 fields (date,category,amount) but found 2",
    "line 4: the amount \"ten\" is not a number"
  ]
end

test "blank lines and spaces around fields don't count"
  csv = "date,category,amount\n\n 2026-10-01 , food , 1.50 \n\n"
  console = Console.fake()
  files = Files.in_memory({"october.csv": csv}).read_only
  assert main(Args.of(["october.csv"]), console, files) == 0
  assert console.printed == ["food  1.50"]
  assert console.printed_errors == []
end

test "without exactly one file name, the usage is printed and the status is 2"
  console = Console.fake()
  files = Files.in_memory({}).read_only
  assert main(Args.of([]), console, files) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: expenses FILE"]
end

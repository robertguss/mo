## Totals a CSV file of expenses by category, largest first.
##
## Usage: expenses FILE
##
## The file's first line is a header, and every other line is
## date,category,amount, as in `2026-10-12,groceries,12.50`. Spaces around a
## field don't count, and blank lines are ignored. Each category's total is
## printed on standard output, largest first. A malformed line is reported on
## standard error with its line number and skipped, and the program then exits
## with status 1. A file that can't be read also gives status 1, and a wrong
## number of arguments gives status 2.
module Expenses

## One line of the file.
struct Expense
  date: Text
  category: Text
  amount: Decimal
end

## Why a line is malformed.
enum Problem
  WrongFieldCount(found: Int)
  MissingDate
  MissingCategory
  MissingAmount
  NotAnAmount(text: Text)
end

## A malformed line: its number, counting the header as line 1, and what is
## wrong with it.
struct BadLine
  number: Int
  problem: Problem
end

## What one category's expenses add up to.
struct Total
  category: Text
  amount: Decimal
end

## What a file holds: the totals, largest first, and the lines skipped.
struct Summary
  totals: List(Total)
  bad_lines: List(BadLine)
end

## Prints the totals of the file named on the command line, and reports its
## malformed lines. Exits with 0 when every line was read, 1 when a line was
## skipped or the file couldn't be read, and 2 unless exactly one file is named.
fn main(args: Args, console: Console, files: ReadFiles) : Int
  path = case args.list
    [given]: given
    _:
      console.error("usage: expenses FILE")
      return 2
  end
  csv = case files.read_text(path)
    Ok(text): text
    Error(failure):
      console.error("#{failure}")
      return 1
  end
  summary = summarize(csv)
  for bad_line in summary.bad_lines
    console.error("line #{bad_line.number} skipped: #{bad_line.problem}")
  end
  for row in report(summary.totals)
    console.print(row)
  end
  if summary.bad_lines.empty?: 0 else: 1
end

## Reads a CSV text whose first line is a header, and totals its expenses.
## Blank lines are passed over; a malformed line is skipped and noted.
fn summarize(csv: Text) : Summary
  var expenses = []
  var bad_lines = []
  for (index, line) in csv.lines.enumerate
    continue if index == 0            # the header
    continue if line.trim.empty?      # a blank line holds no expense
    case parse_expense(line)
      Ok(expense): expenses = expenses.append(expense)
      Error(problem):
        bad_lines = bad_lines.append(BadLine(number: index + 1, problem: problem))
    end
  end
  Summary(totals: totals_by_category(expenses), bad_lines: bad_lines)
end

## Reads one line after the header: date,category,amount, with any spaces
## around a field dropped.
fn parse_expense(line: Text) : Result(Expense, Problem)
  promise "an expense has a date and a category, without spaces around them"
    result is Ok(expense) implies not expense.date.empty? and
      expense.date == expense.date.trim and not expense.category.empty? and
      expense.category == expense.category.trim
    allows line: "2026-10-12,rent,950",
      result: Ok(Expense(date: "2026-10-12", category: "rent", amount: 950))
    forbids line: "2026-10-12, rent ,950",
      result: Ok(Expense(date: "2026-10-12", category: " rent ", amount: 950))
  end
  examples
    parse_expense("2026-10-12,groceries,12.50") ==
      Ok(Expense(date: "2026-10-12", category: "groceries", amount: 12.50))
    parse_expense(" 2026-10-12 , rent , 950 ") ==
      Ok(Expense(date: "2026-10-12", category: "rent", amount: 950))
    parse_expense("2026-10-12,groceries") == Error(WrongFieldCount(found: 2))
    parse_expense("2026-10-12,groceries,1,250.00") == Error(WrongFieldCount(found: 4))
    parse_expense(",groceries,12.50") == Error(MissingDate)
    parse_expense("2026-10-12, ,12.50") == Error(MissingCategory)
    parse_expense("2026-10-12,groceries,") == Error(MissingAmount)
    parse_expense("2026-10-12,groceries,$12.50") == Error(NotAnAmount(text: "$12.50"))
  end

  fields = line.split(",").map(fn(field) field.trim end)
  case fields
    [date, category, amount_text]:
      return Error(MissingDate) if date.empty?
      return Error(MissingCategory) if category.empty?
      return Error(MissingAmount) if amount_text.empty?
      amount = try amount_text.to_decimal.or_fail(NotAnAmount(text: amount_text))
      Ok(Expense(date: date, category: category, amount: amount))
    _: Error(WrongFieldCount(found: fields.size))
  end
end

## Each category's total, largest first. Equal totals come in the order of
## their categories' names.
fn totals_by_category(expenses: List(Expense)) : List(Total)
  examples
    totals_by_category([]) == []
    totals_by_category([
      Expense(date: "2026-10-01", category: "food", amount: 12.50),
      Expense(date: "2026-10-02", category: "rent", amount: 950),
      Expense(date: "2026-10-03", category: "food", amount: 7.25),
      Expense(date: "2026-10-04", category: "bus", amount: 19.75)
    ]) == [
      Total(category: "rent", amount: 950),
      Total(category: "bus", amount: 19.75),
      Total(category: "food", amount: 19.75)
    ]
  end

  var sums = {}
  for expense in expenses
    sums = sums.update(expense.category, 0, fn(sum) sum + expense.amount end)
  end
  # Sorting is stable, so sorting by name first orders the equal totals.
  sums.entries
    .map(fn(entry) Total(category: entry.0, amount: entry.1) end)
    .sort_by(fn(total) total.category end)
    .sort_by_desc(fn(total) total.amount end)
end

## The lines to print: each category and its total, in two aligned columns.
fn report(totals: List(Total)) : List(Text)
  examples
    report([]) == []
    report([
      Total(category: "rent", amount: 950),
      Total(category: "groceries", amount: 42.75)
    ]) == [
      "rent       950.00",
      "groceries   42.75"
    ]
  end

  category_width = totals.map(fn(total) total.category.char_count end).max or 0
  amount_width = totals.map(fn(total) format_amount(total.amount).char_count end).max or 0
  totals.map(fn(total)
    category = total.category.pad_right(category_width)
    amount = format_amount(total.amount).pad_left(amount_width)
    "#{category}  #{amount}"
  end)
end

## An amount as the report shows it: with two decimal places, as money is
## written, or with all of its places when it has more, so that nothing is
## rounded away.
fn format_amount(amount: Decimal) : Text
  examples
    format_amount(12.5) == "12.50"
    format_amount(950) == "950.00"
    format_amount(-3.1) == "-3.10"
    format_amount(0.125) == "0.125"
  end

  # Not amount.round(2) == amount: rounding up could overflow at the top of
  # Decimal's range, and a remainder can't.
  whole_cents? = amount % 0.01 == 0
  if whole_cents?: amount.to_fixed(2) else: "#{amount}"
end

## How a problem is described in the report of a malformed line.
fn to_text(problem: Problem) : Text
  case problem
    WrongFieldCount(found: count):
      "expected 3 fields, date,category,amount, but found #{count}"
    MissingDate: "the date is missing"
    MissingCategory: "the category is missing"
    MissingAmount: "the amount is missing"
    NotAnAmount(text: amount): "the amount \"#{amount}\" is not a number such as 12.50"
  end
end

test "expenses are totaled by category, largest first"
  csv = """
    date,category,amount
    2026-10-01,groceries,12.50
    2026-10-02,rent,950.00
    2026-10-03,groceries,30.25
    2026-10-04,transport,2.75
    """
  summary = summarize(csv)
  assert summary.totals == [
    Total(category: "rent", amount: 950),
    Total(category: "groceries", amount: 42.75),
    Total(category: "transport", amount: 2.75)
  ]
  assert summary.bad_lines.empty?
end

test "a malformed line is reported with its line number and skipped"
  csv = """
    date,category,amount
    2026-10-01,groceries,12.50
    2026-10-02,rent
    2026-10-03,groceries,twelve
    2026-10-04,,3.00
    2026-10-05,groceries,1.50
    """
  summary = summarize(csv)
  assert summary.totals == [Total(category: "groceries", amount: 14)]
  assert summary.bad_lines == [
    BadLine(number: 3, problem: WrongFieldCount(found: 2)),
    BadLine(number: 4, problem: NotAnAmount(text: "twelve")),
    BadLine(number: 5, problem: MissingCategory)
  ]
end

test "blank lines and Windows line endings are fine"
  summary = summarize("date,category,amount\r\n2026-10-01, rent ,950\r\n\r\n2026-10-02,rent,50\r\n")
  assert summary.totals == [Total(category: "rent", amount: 1000)]
  assert summary.bad_lines.empty?
end

test "a file with only its header has no totals"
  summary = summarize("date,category,amount\n")
  assert summary.totals.empty?
  assert summary.bad_lines.empty?
end

test "main prints the totals, reports a malformed line, and exits with 1"
  console = Console.fake()
  csv = "date,category,amount\n2026-10-01,food,12.50\noops\n2026-10-02,rent,950\n"
  files = Files.in_memory({"expenses.csv": csv}).read_only
  assert main(Args.of(["expenses.csv"]), console, files) == 1
  assert console.printed == [
    "rent  950.00",
    "food   12.50"
  ]
  assert console.printed_errors == [
    "line 3 skipped: expected 3 fields, date,category,amount, but found 1"
  ]
end

test "main exits with 0 when every line is well formed"
  console = Console.fake()
  csv = "date,category,amount\n2026-10-01,food,12.50\n"
  files = Files.in_memory({"expenses.csv": csv}).read_only
  assert main(Args.of(["expenses.csv"]), console, files) == 0
  assert console.printed == ["food  12.50"]
  assert console.printed_errors.empty?
end

test "a file that can't be read is reported, with status 1"
  console = Console.fake()
  assert main(Args.of(["missing.csv"]), console, Files.in_memory({}).read_only) == 1
  assert console.printed_errors == ["missing.csv: no such file"]
  assert console.printed.empty?
end

test "main needs exactly one file name"
  console = Console.fake()
  assert main(Args.of([]), console, Files.in_memory({}).read_only) == 2
  assert console.printed_errors == ["usage: expenses FILE"]
end

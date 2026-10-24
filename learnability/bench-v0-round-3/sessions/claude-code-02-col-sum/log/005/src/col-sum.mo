## Totals each column of a CSV table of numbers read from standard input.
## The first non-blank line names the columns; each later line is a row.
module ColSum

## Why a row was skipped.
enum RowError
  WrongCount(expected: Int, found: Int)
  NotANumber(field: Text)
end

## The number of units in 1: numbers are kept exactly as whole multiples of 10^-18.
scale = 10.to_big.pow(18)

## Removes the spaces and tabs at both ends of a text.
fn trim_blanks(text: Text) : Text
  examples
    trim_blanks(" \t a b\t ") == "a b"
    trim_blanks("\r1") == "\r1"
    trim_blanks("  ") == ""
  end

  var chars = text.chars
  while chars.first is Some(c) and (c == ' ' or c == '\t')
    chars = chars.drop(1)
  end
  while chars.last is Some(c) and (c == ' ' or c == '\t')
    chars = chars.take(chars.size - 1)
  end
  Text.from_chars(chars)
end

## The value of a nonempty run of ASCII digits, or None for anything else.
fn digits_value(text: Text) : Option(BigInt)
  examples
    digits_value("0012") == Some(12.to_big)
    digits_value("") == None
    digits_value("1a") == None
  end

  return None if text.empty? or not text.chars.all?(fn(c) c.ascii_digit? end)
  case text.to_int
    Some(n): Some(n.to_big)
    None: Some(text.chars.reduce(0.to_big, fn(n, c) n * 10.to_big + (c.code - '0'.code).to_big end))
  end
end

## A field read as a number, in units of 10^-18: an optional `-`, one or more digits,
## and optionally `.` with 1 to 18 digits.
fn parse_number(field: Text) : Option(BigInt)
  examples
    parse_number("3") == Some(3.to_big * scale)
    parse_number("-0.25") == Some(-(25.to_big * 10.to_big.pow(16)))
    parse_number("10.50") == Some(105.to_big * 10.to_big.pow(17))
    parse_number("0.000000000000000001") == Some(1.to_big)
    parse_number("1.0000000000000000001") == None
    parse_number("3.") == None
    parse_number(".5") == None
    parse_number("1e3") == None
    parse_number("") == None
    parse_number("-") == None
    parse_number("--1") == None
    parse_number("+1") == None
    parse_number("1.2.3") == None
  end

  negative? = field.starts_with?("-")
  body = field.drop_prefix("-") or field
  (whole_text, fraction_text) = case body.split(".")
    [w]: (w, "")
    [w, f] if not f.empty? and f.char_count <= 18: (w, f)
    _: return None
  end
  whole = try digits_value(whole_text)
  padded = "#{fraction_text}#{"0".repeat(18 - fraction_text.char_count)}"
  fraction = try digits_value(padded)
  magnitude = whole * scale + fraction
  Some(if negative?: 0.to_big - magnitude else: magnitude)
end

## The fields of a row, read as numbers, or why the row is skipped.
fn parse_row(line: Text, expected: Int) : Result(List(BigInt), RowError)
  examples
    parse_row(" 1 ,\t2", 2) == Ok([scale, 2.to_big * scale])
    parse_row("1,2,3", 2) == Error(WrongCount(expected: 2, found: 3))
    parse_row("x,2,y", 3) == Error(NotANumber(field: "x"))
    parse_row("1, 2 x ,y", 3) == Error(NotANumber(field: "2 x"))
    parse_row("x", 2) == Error(WrongCount(expected: 2, found: 1))
  end

  fields = line.split(",").map(trim_blanks)
  return Error(WrongCount(expected: expected, found: fields.size)) if fields.size != expected
  var numbers = []
  for field in fields
    case parse_number(field)
      Some(n): numbers = numbers.append(n)
      None: return Error(NotANumber(field: field))
    end
  end
  Ok(numbers)
end

## How a skipped row is reported, without its line number.
fn describe(problem: RowError) : Text
  case problem
    WrongCount(expected: k, found: m): "expected #{k} fields, found #{m}"
    NotANumber(field: f): "not a number: #{f}"
  end
end

## A total in units of 10^-18, written with two decimal places, a half rounded away from zero.
fn format_total(total: BigInt) : Text
  examples
    format_total(0.to_big) == "0.00"
    format_total(1450.to_big * 10.to_big.pow(16)) == "14.50"
    format_total(5.to_big * 10.to_big.pow(15)) == "0.01"
    format_total(0.to_big - 5.to_big * 10.to_big.pow(15)) == "-0.01"
    format_total(4.to_big * 10.to_big.pow(15)) == "0.00"
    format_total(0.to_big - 4.to_big * 10.to_big.pow(15)) == "0.00"
    format_total(0.to_big - 123456.to_big * 10.to_big.pow(16)) == "-1234.56"
    format_total(7.to_big * scale) == "7.00"
  end

  unit = 10.to_big.pow(16)
  cents = (total.abs + unit / 2.to_big) / unit
  whole = cents / 100.to_big
  part = cents % 100.to_big
  padding = if part < 10.to_big: "0" else: ""
  sign = if total < 0.to_big and cents > 0.to_big: "-" else: ""
  "#{sign}#{whole}.#{padding}#{part}"
end

## A line that is empty or holds only white space.
fn blank?(line: Text) : Bool
  line.trim.empty?
end

fn main(console: Console) : Int
  var line_number = 0
  var header = None
  while header == None and console.read_line is Some(line)
    line_number += 1
    if not blank?(line)
      header = Some(line.split(",").map(trim_blanks))
    end
  end
  names = case header
    Some(n): n
    None:
      console.error("no header")
      return 1
  end
  var totals = names.map(fn(_) 0.to_big end)
  var status = 0
  while console.read_line is Some(line)
    line_number += 1
    continue if blank?(line)
    case parse_row(line, names.size)
      Ok(numbers): totals = totals.zip(numbers).map(fn(pair) pair.0 + pair.1 end)
      Error(problem):
        console.error("line #{line_number}: #{describe(problem)}")
        status = 1
    end
  end
  for (name, total) in names.zip(totals)
    console.print("#{name}: #{format_total(total)}")
  end
  status
end

test "the example table is totalled"
  console = Console.fake_input(["item, price, tax", "1, 4.50, 0.90", "2, 10, 2"])
  assert main(console) == 0
  assert console.printed == ["item: 3.00", "price: 14.50", "tax: 2.90"]
  assert console.printed_errors == []
end

test "bad rows are reported, skipped and counted with blank lines"
  console = Console.fake_input(["a,b", "", "1,2,3", "  \t", "x, y", "1.5, -2", "3.,1"])
  assert main(console) == 1
  assert console.printed == ["a: 1.50", "b: -2.00"]
  assert console.printed_errors == [
    "line 3: expected 2 fields, found 3",
    "line 5: not a number: x",
    "line 7: not a number: 3."
  ]
end

test "no input at all has no header"
  console = Console.fake_input([])
  assert main(console) == 1
  assert console.printed == []
  assert console.printed_errors == ["no header"]
end

test "a header with no rows totals zero"
  console = Console.fake_input(["only"])
  assert main(console) == 0
  assert console.printed == ["only: 0.00"]
end

test "totals beyond Decimal's range stay exact"
  big = "99999999999999999999999.995"
  console = Console.fake_input(["n", big, big])
  assert main(console) == 0
  assert console.printed == ["n: 199999999999999999999999.99"]
end

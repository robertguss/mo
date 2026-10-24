## Totals each column of a CSV table of numbers read from standard input.
module ColSum

## One unit of a scaled number: numbers are held exactly as multiples of 10^-18.
fn scale() : BigInt
  10.to_big.pow(18)
end

## Drops spaces and tabs from both ends of a text.
fn strip(text: Text) : Text
  examples
    strip(" \tab c\t ") == "ab c"
    strip("   ") == ""
    strip("\rx") == "\rx"
  end

  chars = text.chars
  var start = 0
  while start < chars.size and blank_char?(chars.get(start) or 'x')
    start += 1
  end
  var finish = chars.size
  while finish > start and blank_char?(chars.get(finish - 1) or 'x')
    finish -= 1
  end
  Text.from_chars(chars.drop(start).take(finish - start))
end

fn blank_char?(c: Char) : Bool
  c == ' ' or c == '\t'
end

## The value of a non-empty run of ASCII digits, or None for anything else.
fn digits_value(text: Text) : Option(BigInt)
  examples
    digits_value("0042") == Some(42.to_big)
    digits_value("") == None
    digits_value("4a") == None
  end

  return None if text.empty?
  var value = 0.to_big
  for c in text.chars
    return None if not c.ascii_digit?
    value = value * 10.to_big + (c.code - '0'.code).to_big
  end
  Some(value)
end

## A field's number, as a multiple of 10^-18, or None when the field isn't a number.
fn parse_number(field: Text) : Option(BigInt)
  examples
    parse_number("3") == Some(3.to_big * scale())
    parse_number("-0.25") == Some(-(25.to_big) * 10.to_big.pow(16))
    parse_number("10.50") == Some(105.to_big * 10.to_big.pow(17))
    parse_number("0.000000000000000001") == Some(1.to_big)
    parse_number("0.0000000000000000001") == None
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
  magnitude = case body.split(".")
    [whole]: try digits_value(whole) * scale()
    [whole, fraction] if fraction.char_count <= 18:
      whole_value = try digits_value(whole)
      fraction_value = try digits_value(fraction)
      whole_value * scale() + fraction_value * 10.to_big.pow(18 - fraction.char_count)
    _: return None
  end
  Some(if negative?: -magnitude else: magnitude)
end

## A scaled total written with two decimal places, a half rounded away from zero.
fn format_total(total: BigInt) : Text
  examples
    format_total(0.to_big) == "0.00"
    format_total(145.to_big * 10.to_big.pow(17)) == "14.50"
    format_total(5.to_big * 10.to_big.pow(15)) == "0.01"
    format_total(-(5.to_big) * 10.to_big.pow(15)) == "-0.01"
    format_total(4999.to_big * 10.to_big.pow(12)) == "0.00"
    format_total(-(1.to_big)) == "0.00"
    format_total(-(1234.to_big) * 10.to_big.pow(16)) == "-12.34"
    format_total(10.to_big.pow(40)) == "10000000000000000000000.00"
  end

  negative? = total < 0.to_big
  magnitude = total.abs
  unit = 10.to_big.pow(16)
  remainder = magnitude % unit
  cents = magnitude / unit + (if remainder * 2.to_big >= unit: 1.to_big else: 0.to_big)
  hundred = 100.to_big
  part = cents % hundred
  padding = if part < 10.to_big: "0" else: ""
  sign = if negative? and cents > 0.to_big: "-" else: ""
  "#{sign}#{cents / hundred}.#{padding}#{part}"
end

## Whether a line is empty or holds only white space.
fn blank_line?(line: Text) : Bool
  line.chars.all?(fn(c) c.ascii_white_space? end)
end

## A row's numbers, or the message that reports why the row is skipped.
fn parse_row(line: Text, number: Int, columns: Int) : Result(List(BigInt), Text)
  examples
    parse_row(" 1 ,\t-2.5", 2, 2) == Ok([scale(), -(25.to_big) * 10.to_big.pow(17)])
    parse_row("1,2,3", 4, 2) == Error("line 4: expected 2 fields, found 3")
    parse_row("x, 2, y", 3, 3) == Error("line 3: not a number: x")
    parse_row("1, 2.,", 5, 3) == Error("line 5: not a number: 2.")
    parse_row("1,,3", 6, 3) == Error("line 6: not a number: ")
  end

  fields = line.split(",").map(strip)
  if fields.size != columns
    return Error("line #{number}: expected #{columns} fields, found #{fields.size}")
  end
  var values = []
  for field in fields
    case parse_number(field)
      Some(value): values = values.append(value)
      None: return Error("line #{number}: not a number: #{field}")
    end
  end
  Ok(values)
end

fn main(console: Console) : Int
  var number = 0
  var header = None
  while header == None
    case console.read_line
      Some(line):
        number += 1
        if not blank_line?(line)
          header = Some(line.split(",").map(strip))
        end
      None:
        console.error("no header")
        return 1
    end
  end
  names = header or []
  var totals = names.map(fn(_) 0.to_big end)
  var status = 0
  while console.read_line is Some(line)
    number += 1
    continue if blank_line?(line)
    case parse_row(line, number, names.size)
      Ok(values): totals = totals.zip(values).map(fn(pair) pair.0 + pair.1 end)
      Error(message):
        console.error(message)
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

test "bad rows are reported and skipped, and blank lines are counted"
  console = Console.fake_input(["a,b", "", "1,2,3", "  \t", "x,1", "1, 2y", "0.5,-1.25", "2,3"])
  assert main(console) == 1
  assert console.printed == ["a: 2.50", "b: 1.75"]
  assert console.printed_errors == [
    "line 3: expected 2 fields, found 3",
    "line 5: not a number: x",
    "line 6: not a number: 2y"
  ]
end

test "no input at all has no header"
  console = Console.fake_input([])
  assert main(console) == 1
  assert console.printed == []
  assert console.printed_errors == ["no header"]
end

test "a header alone totals every column to zero"
  console = Console.fake_input([" first ,second"])
  assert main(console) == 0
  assert console.printed == ["first: 0.00", "second: 0.00"]
end

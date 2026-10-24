## Totals each column of a CSV table of numbers read from standard input.
module ColSum

## A number's value in units of 10^-18, so that sums are exact.
fn scale() : BigInt
  10.to_big.pow(18)
end

## Drops spaces and tabs from both ends of a text.
fn strip(text: Text) : Text
  examples
    strip(" \tab c\t ") == "ab c"
    strip("   ") == ""
  end

  chars = text.chars
  var start = 0
  while chars.get(start) is Some(c) and (c == ' ' or c == '\t')
    start += 1
  end
  var stop_at = chars.size
  while stop_at > start and chars.get(stop_at - 1) is Some(c) and (c == ' ' or c == '\t')
    stop_at -= 1
  end
  Text.from_chars(chars.drop(start).take(stop_at - start))
end

## Whether a line is empty or holds only white space.
fn blank?(line: Text) : Bool
  examples
    blank?("")
    blank?(" \t ")
    not blank?(" , ")
  end

  line.chars.all?(fn(c) c.ascii_white_space? end)
end

## The value of a run of ASCII digits, or None when the text is empty or holds anything else.
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

## Reads a field as a number, scaled by 10^18.
fn parse_number(field: Text) : Option(BigInt)
  examples
    parse_number("3") == Some(3.to_big * scale())
    parse_number("-0.25") == Some(-(25.to_big * 10.to_big.pow(16)))
    parse_number("10.50") == Some(105.to_big * 10.to_big.pow(17))
    parse_number("3.") == None
    parse_number(".5") == None
    parse_number("1e3") == None
    parse_number("") == None
    parse_number("-") == None
    parse_number("1.2.3") == None
    parse_number("0.1234567890123456789") == None
  end

  negative? = field.starts_with?("-")
  body = field.drop_prefix("-") or field
  parts = body.split(".")
  (whole_text, fraction) = case parts
    [w]: (w, "")
    [w, f] if not f.empty? and f.char_count <= 18: (w, f)
    _: return None
  end
  whole = try digits_value(whole_text)
  fraction_value = if fraction.empty?
    0.to_big
  else
    try digits_value(fraction + "0".repeat(18 - fraction.char_count))
  end
  value = whole * scale() + fraction_value
  Some(if negative?: 0.to_big - value else: value)
end

## Writes a scaled total with exactly two decimal places, a half rounded away from zero.
fn format_total(total: BigInt) : Text
  examples
    format_total(0.to_big) == "0.00"
    format_total(145.to_big * 10.to_big.pow(17)) == "14.50"
    format_total(5.to_big * 10.to_big.pow(15)) == "0.01"
    format_total(0.to_big - 5.to_big * 10.to_big.pow(15)) == "-0.01"
    format_total(0.to_big - 4.to_big * 10.to_big.pow(15)) == "0.00"
    format_total(0.to_big - 1999.to_big * 10.to_big.pow(16)) == "-19.99"
  end

  unit = 10.to_big.pow(16)
  magnitude = total.abs
  remainder = magnitude % unit
  hundredths = magnitude / unit + (if remainder * 2.to_big >= unit: 1.to_big else: 0.to_big)
  cents = case (hundredths % 100.to_big).to_int
    Some(n): n
    None: stop "a remainder below 100 fits in an Int"
  end
  sign = if total < 0.to_big and hundredths > 0.to_big: "-" else: ""
  padded = if cents < 10: "0#{cents}" else: "#{cents}"
  "#{sign}#{hundredths / 100.to_big}.#{padded}"
end

## What is wrong with a row, if anything; otherwise its numbers.
fn parse_row(line: Text, line_number: Int, width: Int) : Result(List(BigInt), Text)
  fields = line.split(",").map(strip)
  if fields.size != width
    return Error("line #{line_number}: expected #{width} fields, found #{fields.size}")
  end
  var values = []
  for field in fields
    case parse_number(field)
      Some(v): values = values.append(v)
      None: return Error("line #{line_number}: not a number: #{field}")
    end
  end
  Ok(values)
end

fn main(console: Console) : Int
  var line_number = 0
  var header = None
  while header == None and console.read_line is Some(line)
    line_number += 1
    if not blank?(line)
      header = Some(line.split(",").map(strip))
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
    case parse_row(line, line_number, names.size)
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

test "bad rows are reported and skipped"
  console = Console.fake_input(["a,b", "", "1,2,3", "x, 2", "1, y", " \t", "0.005, -0.005", "1"])
  assert main(console) == 1
  assert console.printed == ["a: 0.01", "b: -0.01"]
  assert console.printed_errors == [
    "line 3: expected 2 fields, found 3",
    "line 4: not a number: x",
    "line 5: not a number: y",
    "line 8: expected 2 fields, found 1"
  ]
end

test "no input means no header"
  console = Console.fake_input([])
  assert main(console) == 1
  assert console.printed == []
  assert console.printed_errors == ["no header"]
end

test "a header with no rows totals zero"
  console = Console.fake_input(["x"])
  assert main(console) == 0
  assert console.printed == ["x: 0.00"]
end

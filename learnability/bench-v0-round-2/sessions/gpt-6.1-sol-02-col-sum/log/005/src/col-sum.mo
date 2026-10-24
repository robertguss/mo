## Totals CSV columns exactly and rounds only the final sums.
module ColSum

scale = 10.to_big.pow(18)

fn trim_field(text: Text) : Text
  chars = text.chars
  var first = 0
  var last = chars.size
  while first < last
    c = chars.get(first) or stop "first is within the characters"
    break if c != ' ' and c != '\t'
    first += 1
  end
  while last > first
    c = chars.get(last - 1) or stop "last is within the characters"
    break if c != ' ' and c != '\t'
    last -= 1
  end
  Text.from_chars(chars.drop(first).take(last - first))
end

fn parse_number(field: Text) : Option(BigInt)
  negative? = field.starts_with?("-")
  digits = if negative?: field.drop_prefix("-") or "" else: field
  var value = 0.to_big
  var whole_digits = 0
  var fraction_digits = 0
  var dot? = false
  for c in digits.chars
    if c == '.'
      return None if dot? or whole_digits == 0
      dot? = true
    else
      return None if not c.ascii_digit?
      value = value * 10 + (c.code - '0'.code).to_big
      if dot?
        fraction_digits += 1
        return None if fraction_digits > 18
      else
        whole_digits += 1
      end
    end
  end
  return None if whole_digits == 0 or (dot? and fraction_digits == 0)
  scaled = value * 10.to_big.pow(18 - fraction_digits)
  Some(if negative?: -scaled else: scaled)
end

fn parse_row(fields: List(Text)) : Result(List(BigInt), Text)
  var values = []
  for field in fields
    case parse_number(field)
      Some(value): values = values.append(value)
      None: return Error(field)
    end
  end
  Ok(values)
end

fn format_total(total: BigInt) : Text
  cents = (total.abs + scale / 200) / (scale / 100)
  sign = if total < 0 and cents != 0: "-" else: ""
  fraction = cents % 100
  padding = if fraction < 10: "0" else: ""
  "#{sign}#{cents / 100}.#{padding}#{fraction}"
end

fn main(console: Console) : Int
  var names = []
  var totals = []
  var line_number = 0
  var status = 0
  while console.read_line is Some(line)
    line_number += 1
    continue if line.trim.empty?
    fields = line.split(",").map(trim_field)
    if names.empty?
      names = fields
      totals = fields.map(fn(_field) 0.to_big end)
      continue
    end
    if fields.size != names.size
      console.error("line #{line_number}: expected #{names.size} fields, found #{fields.size}")
      status = 1
      continue
    end
    case parse_row(fields)
      Ok(values): totals = totals.zip(values).map(fn(pair) pair.0 + pair.1 end)
      Error(field):
        console.error("line #{line_number}: not a number: #{field}")
        status = 1
    end
  end
  if names.empty?
    console.error("no header")
    return 1
  end
  for (name, total) in names.zip(totals)
    console.print("#{name}: #{format_total(total)}")
  end
  status
end

test "the sample trims names and fields and preserves column order"
  console = Console.fake_input(["item, price,\ttax\t", "1, 4.50, 0.90", "2,\t10\t, 2"])
  assert main(console) == 0
  assert console.printed == ["item: 3.00", "price: 14.50", "tax: 2.90"]
  assert console.printed_errors == []
end

test "bad rows contribute nothing and diagnostics count blank lines"
  console = Console.fake_input([
    "a,b",
    "1,2",
    "",
    "900,bad",
    "x,nope",
    "bad",
    "5,6,7",
    ",8",
    "9,",
    "2,3"
  ])
  assert main(console) == 1
  assert console.printed == ["a: 3.00", "b: 5.00"]
  assert console.printed_errors == [
    "line 4: not a number: bad",
    "line 5: not a number: x",
    "line 6: expected 2 fields, found 1",
    "line 7: expected 2 fields, found 3",
    "line 8: not a number: ",
    "line 9: not a number: "
  ]
end

test "only the specified ASCII number grammar is accepted"
  for field in [
    "3.",
    ".5",
    "1e3",
    "",
    "+1",
    "--1",
    "-",
    "1.2.3",
    "1.1234567890123456789",
    "١",
    "1 2",
    "1\t2",
    "\u{B}3",
    "3\u{C}"
  ]
    console = Console.fake_input(["a,b", "#{field},1"])
    assert main(console) == 1
    assert console.printed == ["a: 0.00", "b: 0.00"]
    assert console.printed_errors == ["line 2: not a number: #{field}"]
  end
end

test "rounding happens after exact summation and halves go away from zero"
  console = Console.fake_input([
    "positive,negative,below,below_negative,cancel,carry",
    "0.0025,-0.0025,0.004999999999999999,-0.004999999999999999,1.234,9.995",
    "0.0025,-0.0025,0,0,-1.234,0"
  ])
  assert main(console) == 0
  assert console.printed == [
    "positive: 0.01",
    "negative: -0.01",
    "below: 0.00",
    "below_negative: 0.00",
    "cancel: 0.00",
    "carry: 10.00"
  ]
  assert console.printed_errors == []
end

test "large magnitudes and the eighteenth decimal place remain exact"
  console = Console.fake_input([
    "large,cancellation,boundary",
    "1234567890123456789012345678901234567890.999999999999999999,999999999999999999999999999999.005000000000000001,0.004999999999999999",
    "0.000000000000000001,-999999999999999999999999999999,0.000000000000000001"
  ])
  assert main(console) == 0
  assert console.printed == [
    "large: 1234567890123456789012345678901234567891.00",
    "cancellation: 0.01",
    "boundary: 0.01"
  ]
  assert console.printed_errors == []
end

test "missing headers report an error without totals"
  for lines in [[], ["", " \t", "\u{B}\u{C}"]]
    console = Console.fake_input(lines)
    assert main(console) == 1
    assert console.printed == []
    assert console.printed_errors == ["no header"]
  end
end

test "blank lines are skipped and header-only columns have zero totals"
  console = Console.fake_input(["", "\t ", " x , x, ", " \t", ""])
  assert main(console) == 0
  assert console.printed == ["x: 0.00", "x: 0.00", ": 0.00"]
  assert console.printed_errors == []
end

test "leading zeros and signed zeros are numbers"
  console = Console.fake_input(["zero,number", "-000.000,00010.50", "0,-0000.25"])
  assert main(console) == 0
  assert console.printed == ["zero: 0.00", "number: 10.25"]
  assert console.printed_errors == []
end

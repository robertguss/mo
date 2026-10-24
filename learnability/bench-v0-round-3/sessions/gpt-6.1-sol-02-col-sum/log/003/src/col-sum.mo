## Totals CSV columns exactly, rounding only the final sums.
module ColSum

fn trim_field(text: Text) : Text
  chars = text.chars
  var first = 0
  var last = chars.size
  while first < last
    c = chars.get(first) or '\n'
    break if c != ' ' and c != '\t'
    first += 1
  end
  while last > first
    c = chars.get(last - 1) or '\n'
    break if c != ' ' and c != '\t'
    last -= 1
  end
  Text.from_chars(chars.drop(first).take(last - first))
end

## Parses a number as an integer count of 10^-18 units.
fn parse_number(text: Text) : Option(BigInt)
  negative? = text.starts_with?("-")
  unsigned = text.drop_prefix("-") or text
  var value = 0.to_big
  var whole_digits = 0
  var fraction_digits = 0
  var decimal? = false
  for c in unsigned.chars
    if c.ascii_digit?
      value = value * 10 + (c.code - '0'.code).to_big
      if decimal?
        fraction_digits += 1
        return None if fraction_digits > 18
      else
        whole_digits += 1
      end
    else if c == '.' and not decimal?
      decimal? = true
    else
      return None
    end
  end
  return None if whole_digits == 0 or (decimal? and fraction_digits == 0)
  scaled = value * 10.to_big.pow(18 - fraction_digits)
  Some(if negative?: -scaled else: scaled)
end

fn format_total(total: BigInt) : Text
  cents = (total.abs + 5.to_big * 10.to_big.pow(15)) / 10.to_big.pow(16)
  sign = if total < 0 and cents != 0: "-" else: ""
  fraction = cents % 100
  padding = if fraction < 10: "0" else: ""
  "#{sign}#{cents / 100}.#{padding}#{fraction}"
end

fn main(console: Console) : Int
  header = case console.read_line
    Some(line): line.split(",").map(trim_field)
    None:
      console.error("no header")
      return 1
  end
  var totals = header.map(fn(_name) 0.to_big end)
  var line_number = 1
  var status = 0
  while console.read_line is Some(line)
    line_number += 1
    continue if line.trim.empty?
    fields = line.split(",").map(trim_field)
    if fields.size != header.size
      console.error("line #{line_number}: expected #{header.size} fields, found #{fields.size}")
      status = 1
      continue
    end
    var values = []
    var valid? = true
    for field in fields
      case parse_number(field)
        Some(number): values = values.append(number)
        None:
          console.error("line #{line_number}: not a number: #{field}")
          status = 1
          valid? = false
          break
      end
    end
    continue if not valid?
    totals = totals.zip(values).map(fn(pair) pair.0 + pair.1 end)
  end
  for (name, total) in header.zip(totals)
    console.print("#{name}: #{format_total(total)}")
  end
  status
end

test "the sample totals columns in header order"
  console = Console.fake_input(["item, price, tax", "1, 4.50, 0.90", "2, 10, 2"])
  assert main(console) == 0
  assert console.printed == ["item: 3.00", "price: 14.50", "tax: 2.90"]
  assert console.printed_errors == []
end

test "bad rows contribute nothing and diagnostics use physical line numbers"
  console = Console.fake_input(["a,b", "1,2", "100,bad", " \t", "oops,bad", "bad,2,3", "3,4", ""])
  assert main(console) == 1
  assert console.printed == ["a: 4.00", "b: 6.00"]
  assert console.printed_errors == [
    "line 3: not a number: bad",
    "line 5: not a number: oops",
    "line 6: expected 2 fields, found 3"
  ]
end

test "only the specified number grammar is accepted"
  for invalid in [
    "",
    "-",
    "+1",
    ".5",
    "3.",
    "1e3",
    "1_000",
    "--1",
    "1..2",
    "1.0000000000000000000",
    "١",
    "1 2",
    "1\t2",
    "\u{B}1",
    "1\r"
  ]
    console = Console.fake_input(["a,b", "#{invalid},bad"])
    assert main(console) == 1
    assert console.printed == ["a: 0.00", "b: 0.00"]
    assert console.printed_errors == ["line 2: not a number: #{invalid}"]
  end
end

test "exact sums round once with ties away from zero"
  console = Console.fake_input([
    "positive,negative,below,above,zero,carry",
    "0.0025,-0.0025,0.004999999999999999,0.005000000000000001,-0.004999999999999999,9.995",
    "0.0025,-0.0025,0,0,0,0"
  ])
  assert main(console) == 0
  assert console.printed == [
    "positive: 0.01",
    "negative: -0.01",
    "below: 0.00",
    "above: 0.01",
    "zero: 0.00",
    "carry: 10.00"
  ]
  assert console.printed_errors == []
end

test "whole numbers and totals are not limited to Decimal or Int ranges"
  console = Console.fake_input([
    "large,cancel",
    "999999999999999999999999999999.999,1000000000000000000000000000000.01",
    "0.001,-1000000000000000000000000000000"
  ])
  assert main(console) == 0
  assert console.printed == ["large: 1000000000000000000000000000000.00", "cancel: 0.01"]
  assert console.printed_errors == []
end

test "padding is removed and duplicate names remain separate columns"
  console = Console.fake_input([" \tx\t , x , ", " -0003.50 , 0002.500 , -0 ", "1,2,3"])
  assert main(console) == 0
  assert console.printed == ["x: -2.50", "x: 4.50", ": 3.00"]
  assert console.printed_errors == []
end

test "missing input fails while a header alone prints zero totals"
  empty = Console.fake_input([])
  assert main(empty) == 1
  assert empty.printed == []
  assert empty.printed_errors == ["no header"]
  header_only = Console.fake_input([" a, b ", "", " \t"])
  assert main(header_only) == 0
  assert header_only.printed == ["a: 0.00", "b: 0.00"]
  assert header_only.printed_errors == []
end

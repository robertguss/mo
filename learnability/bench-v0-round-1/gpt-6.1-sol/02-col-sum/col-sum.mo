## Totals CSV columns exactly and rounds the final sums to two places.
module ColSum

cent = 10_000_000_000_000_000.to_big

fn trim_field(text: Text) : Text
  chars = text.chars
  var start = 0
  var finish = chars.size
  while chars.get(start) is Some(ch) and (ch == ' ' or ch == '\t')
    start += 1
  end
  while finish > start
    ch = chars.get(finish - 1) or stop "finish is within the field"
    break if ch != ' ' and ch != '\t'
    finish -= 1
  end
  Text.from_chars(chars.drop(start).take(finish - start))
end

fn parse_number(field: Text) : Option(BigInt)
  unsigned = field.drop_prefix("-") or field
  var value = 0.to_big
  var digits = 0
  var fraction = 0
  var point? = false
  for ch in unsigned.chars
    if ch == '.' and not point?
      return None if digits == 0
      point? = true
    else if ch.ascii_digit?
      value = value * 10 + (ch.code - '0'.code).to_big
      digits += 1
      if point?
        fraction += 1
        return None if fraction > 18
      end
    else
      return None
    end
  end
  return None if digits == 0 or (point? and fraction == 0)
  for _ in fraction..18
    value = value * 10
  end
  Some(if field.starts_with?("-"): -value else: value)
end

fn format_total(total: BigInt) : Text
  magnitude = if total < 0: -total else: total
  cents = (magnitude + cent / 2) / cent
  sign = if total < 0 and cents != 0: "-" else: ""
  remainder = cents % 100
  padding = if remainder < 10: "0" else: ""
  "#{sign}#{cents / 100}.#{padding}#{remainder}"
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

fn main(console: Console) : Int
  var names = []
  var line_number = 0
  while console.read_line is Some(line)
    line_number += 1
    continue if line.trim.empty?
    names = line.split(",").map(trim_field)
    break
  end
  if names.empty?
    console.error("no header")
    return 1
  end
  var totals = names.map(fn(_name) 0.to_big end)
  var status = 0
  while console.read_line is Some(line)
    line_number += 1
    continue if line.trim.empty?
    fields = line.split(",").map(trim_field)
    if fields.size != names.size
      console.error("line #{line_number}: expected #{names.size} fields, found #{fields.size}")
      status = 1
      continue
    end
    case parse_row(fields)
      Error(field):
        console.error("line #{line_number}: not a number: #{field}")
        status = 1
      Ok(values): totals = totals.zip(values).map(fn(pair) pair.0 + pair.1 end)
    end
  end
  for (name, total) in names.zip(totals)
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

test "bad rows contribute nothing and report the first problem at its physical line"
  console = Console.fake_input([
    " left , right ",
    "1.25,2.50",
    "\t ",
    "oops,3,4",
    "100,bad",
    "first,second",
    "2,3",
    "4,"
  ])
  assert main(console) == 1
  assert console.printed == ["left: 3.25", "right: 5.50"]
  assert console.printed_errors == [
    "line 4: expected 2 fields, found 3",
    "line 5: not a number: bad",
    "line 6: not a number: first",
    "line 8: not a number: "
  ]
end

test "sums remain exact beyond Decimal range and round only after addition"
  console = Console.fake_input([
    "positive,negative,huge,cancel",
    "0.004999999999999999,-0.004999999999999999,999999999999999999999999999999.999999999999999999,1.005",
    "0.000000000000000001,-0.000000000000000001,0.000000000000000001,-1.005"
  ])
  assert main(console) == 0
  assert console.printed == [
    "positive: 0.01",
    "negative: -0.01",
    "huge: 1000000000000000000000000000000.00",
    "cancel: 0.00"
  ]
  assert console.printed_errors == []
end

test "rounding handles both sides of a half and carries into whole numbers"
  console = Console.fake_input([
    "a,b,c,d,e,f,g,h",
    "1.004999999999999999,1.005,-1.004999999999999999,-1.005,9.995,-9.995,-0.0049,-0"
  ])
  assert main(console) == 0
  assert console.printed == [
    "a: 1.00",
    "b: 1.01",
    "c: -1.00",
    "d: -1.01",
    "e: 10.00",
    "f: -10.00",
    "g: 0.00",
    "h: 0.00"
  ]
  assert console.printed_errors == []
end

test "only the specified number grammar is accepted"
  invalid = [
    "3.",
    ".5",
    "1e3",
    "",
    "+1",
    "--1",
    "-",
    "1.2.3",
    "1.0000000000000000000",
    "１２",
    "1 2",
    "\u{B}2"
  ]
  console = Console.fake_input(["number"].concat(invalid).concat([" \t-0003.50\t ", "0004"]))
  assert main(console) == 1
  assert console.printed == ["number: 0.50"]
  assert console.printed_errors == [
    "line 2: not a number: 3.",
    "line 3: not a number: .5",
    "line 4: not a number: 1e3",
    "line 6: not a number: +1",
    "line 7: not a number: --1",
    "line 8: not a number: -",
    "line 9: not a number: 1.2.3",
    "line 10: not a number: 1.0000000000000000000",
    "line 11: not a number: １２",
    "line 12: not a number: 1 2",
    "line 13: not a number: \u{B}2"
  ]
end

test "header-only input preserves duplicate and empty column names with zero totals"
  console = Console.fake_input([" same ,\t same\t, "])
  assert main(console) == 0
  assert console.printed == ["same: 0.00", "same: 0.00", ": 0.00"]
  assert console.printed_errors == []
end

test "missing headers fail while blank lines are skipped before the header"
  empty = Console.fake_input([])
  assert main(empty) == 1
  assert empty.printed == []
  assert empty.printed_errors == ["no header"]
  blank = Console.fake_input(["", " \t"])
  assert main(blank) == 1
  assert blank.printed == []
  assert blank.printed_errors == ["no header"]
  leading = Console.fake_input(["", " \t", " x ", "bad", "2"])
  assert main(leading) == 1
  assert leading.printed == ["x: 2.00"]
  assert leading.printed_errors == ["line 4: not a number: bad"]
end

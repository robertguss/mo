## Totals CSV columns exactly and rounds the final sums to two places.
module ColSum

cent = 10_000_000_000_000_000.to_big

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
    names = line.split(",").map(fn(name) name.trim end)
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
    fields = line.split(",").map(fn(field) field.trim end)
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

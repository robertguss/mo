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

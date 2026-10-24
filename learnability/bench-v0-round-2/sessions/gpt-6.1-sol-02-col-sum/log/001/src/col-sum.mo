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

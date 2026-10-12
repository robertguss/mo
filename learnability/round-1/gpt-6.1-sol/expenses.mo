## Totals a three-column CSV by category, supporting CSV quoted fields.
## Amounts are signed decimal currency with up to two fractional digits.
module Expenses

fn csv_fields(line: Text) : Option(List(Text))
  var fields = []
  var current = ""
  var quoted? = false
  var closed? = false
  var index = 0
  chars = line.chars
  while index < chars.size
    ch = case chars.get(index)
      Some(value): value
      None: stop "the index is inside the line"
    end
    if quoted?
      if ch == '"'
        if chars.get(index + 1) == Some('"')
          current = "#{current}\""
          index += 1
        else
          quoted? = false
          closed? = true
        end
      else
        current = "#{current}#{ch}"
      end
    else if ch == ','
      fields = fields.append(current)
      current = ""
      closed? = false
    else if ch == '"'
      return None if not current.empty? or closed?
      quoted? = true
    else
      return None if closed?
      current = "#{current}#{ch}"
    end
    index += 1
  end
  return None if quoted?
  Some(fields.append(current))
end

fn cents(text: Text) : Option(BigInt)
  trimmed = text.trim
  negative? = trimmed.starts_with?("-")
  unsigned = if negative?: trimmed.drop_prefix("-") or "" else: trimmed
  pieces = unsigned.split(".")
  (whole, fraction) = case pieces
    [integer]: (integer, "00")
    [integer, digits] if digits.char_count >= 1 and digits.char_count <= 2:
      (integer, "#{digits}#{"0".repeat(2 - digits.char_count)}")
    _: return None
  end
  return None if whole.empty?
  return None if not whole.chars.all?(fn(ch) ch.ascii_digit? end)
  return None if not fraction.chars.all?(fn(ch) ch.ascii_digit? end)
  var amount = 0.to_big
  for ch in "#{whole}#{fraction}".chars
    amount = amount * 10 + (ch.code - '0'.code).to_big
  end
  Some(if negative?: -amount else: amount)
end

fn amount_text(amount: BigInt) : Text
  magnitude = amount.abs
  sign = if amount < 0: "-" else: ""
  "#{sign}#{magnitude / 100}.#{(magnitude % 100).to_text.pad_left(2).replace(" ", "0")}"
end

fn expense(line: Text) : Option((Text, BigInt))
  fields = try csv_fields(line)
  case fields
    [date, category, amount]:
      return None if date.trim.empty? or category.trim.empty?
      parsed = try cents(amount)
      Some((category.trim, parsed))
    _: None
  end
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  path = case args.list
    [name]: name
    _:
      console.error("Usage: expenses FILE.csv")
      return 1
  end
  text = case files.read_text(path)
    Ok(value): value
    Error(problem):
      console.error("#{problem}")
      return 1
  end
  if text.lines.empty?
    console.error("#{path}:1: missing header")
    return 1
  end
  var totals = {}
  var status = 0
  for (index, line) in text.lines.enumerate
    continue if index == 0
    case expense(line)
      Some((category, amount)):
        totals = totals.set(category, (totals.get(category) or 0.to_big) + amount)
      None:
        console.error("#{path}:#{index + 1}: malformed expense")
        status = 1
    end
  end
  for (category, amount) in totals.entries.sort_by_desc(fn(entry) entry.1 end)
    console.print("#{category},#{amount_text(amount)}")
  end
  status
end

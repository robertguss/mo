## Totals a CSV file by category, largest first.
module Expenses

enum CsvState
  FieldStart
  BareField
  QuotedField
  AfterQuote
end

## Reads one physical CSV line; quoted fields may contain commas and doubled quotes.
fn csv_fields(line: Text) : Option(List(Text))
  var fields = []
  var characters = []
  var state = FieldStart
  for character in line.chars
    case state
      FieldStart:
        case character
          ',': fields = fields.append("")
          '"': state = QuotedField
          _:
            characters = characters.append(character)
            state = BareField
        end
      BareField:
        case character
          ',':
            fields = fields.append(Text.from_chars(characters))
            characters = []
            state = FieldStart
          '"': return None
          _: characters = characters.append(character)
        end
      QuotedField:
        if character == '"'
          state = AfterQuote
        else
          characters = characters.append(character)
        end
      AfterQuote:
        case character
          '"':
            characters = characters.append(character)
            state = QuotedField
          ',':
            fields = fields.append(Text.from_chars(characters))
            characters = []
            state = FieldStart
          _: return None
        end
    end
  end
  return None if state == QuotedField
  Some(fields.append(Text.from_chars(characters)))
end

fn parse_expense(line: Text) : Option((Text, Decimal))
  fields = try csv_fields(line)
  case fields
    [date_text, category_text, amount_text]:
      category = category_text.trim
      return None if date_text.trim.empty? or category.empty?
      amount = try amount_text.trim.to_decimal
      Some((category, amount))
    _: None
  end
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  path = case args.list
    [file_path]: file_path
    _:
      console.error("Usage: expenses FILE")
      return 1
  end

  contents = case files.read_text(path)
    Ok(text): text
    Error(problem):
      console.error("#{problem}")
      return 1
  end

  lines = contents.lines
  if lines.empty?
    console.error("#{path}: line 1: missing header")
    return 1
  end

  var totals = {}
  var status = 0
  for (index, line) in lines.enumerate.drop(1)
    case parse_expense(line)
      Some((category, amount)):
        previous = totals.get(category) or 0
        totals = totals.set(category, previous + amount)
      None:
        console.error("#{path}: line #{index + 1}: malformed expense")
        status = 1
    end
  end

  for (category, total) in totals.entries.sort_by_desc(fn(entry) entry.1 end)
    console.print("#{category}: #{total.to_fixed(2)}")
  end
  status
end

## Counts distinct input lines, preserving their first spelling and tie order.
module LineCounts

usage = "usage: line-counts [--ignore-case] [--top N]"

struct Options
  ignore_case?: Bool
  top: Option(Int)
end

fn positive_limit(text: Text) : Option(Int)
  return None if text.empty?
  var value = 0
  for character in text.chars
    return None if not character.ascii_digit?
    value = value.saturating_mul(10).saturating_add(character.code - '0'.code)
  end
  if value >= 1: Some(value) else: None
end

fn parse_options(arguments: List(Text)) : Option(Options)
  var options = Options(ignore_case?: false, top: None)
  var index = 0
  while index < arguments.size
    case arguments.get(index)
      Some("--ignore-case"):
        return None if options.ignore_case?
        options.ignore_case? = true
      Some("--top"):
        return None if options.top is Some(_)
        text = try arguments.get(index + 1)
        limit = try positive_limit(text)
        options.top = Some(limit)
        index += 1
      Some(_): return None
      None: return None
    end
    index += 1
  end
  Some(options)
end

fn main(args: Args, console: Console) : Int
  options = case parse_options(args.list)
    Some(parsed): parsed
    None:
      console.error(usage)
      return 2
  end

  var counts = {}
  while console.read_line is Some(line)
    key = if options.ignore_case?: line.ascii_lowercase else: line
    previous = counts.get(key) or (line, 0)
    counts = counts.set(key, (previous.0, previous.1 + 1))
  end

  ranked = counts.values.sort_by_desc(fn(row) row.1 end).take(options.top or counts.size)
  width = case ranked.first
    Some(row): row.1.to_text.char_count
    None: 0
  end
  for row in ranked
    console.print("#{row.1.to_text.pad_left(width)} #{row.0}")
  end
  0
end

test "frequency ties keep first appearance rather than alphabetical order"
  console = Console.fake_input(["z", "a", "b", "a", "z", "c"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["2 z", "2 a", "1 b", "1 c"]
  assert console.printed_errors == []
end

test "case folding preserves the first spelling and top truncates after sorting"
  console = Console.fake_input(["Error", "other", "ERROR", "error", "other"])
  assert main(Args.of(["--ignore-case", "--top", "1"]), console) == 0
  assert console.printed == ["3 Error"]
  assert console.printed_errors == []
end

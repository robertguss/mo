## Counts input lines, retaining first spelling and first-seen order for ties.
module LineCounts

struct Options
  ignore_case?: Bool
  top: Option(Int)
end

struct CountedLine
  count: Int
  line: Text
end

fn parse_top(text: Text) : Option(Int)
  return None if text.empty?
  var number = 0
  for digit in text.chars
    return None if not digit.ascii_digit?
    number = number.saturating_mul(10).saturating_add(digit.code - '0'.code)
  end
  if number >= 1: Some(number) else: None
end

fn parse_options(arguments: List(Text)) : Option(Options)
  var options = Options(ignore_case?: false, top: None)
  var position = 0
  while arguments.get(position) is Some(argument)
    case argument
      "--ignore-case":
        return None if options.ignore_case?
        options.ignore_case? = true
      "--top":
        return None if options.top is Some(_)
        position += 1
        value = try arguments.get(position)
        options.top = Some(try parse_top(value))
      _: return None
    end
    position += 1
  end
  Some(options)
end

fn main(args: Args, console: Console) : Int
  options = case parse_options(args.list)
    Some(parsed): parsed
    None:
      console.error("usage: line-counts [--ignore-case] [--top N]")
      return 2
  end

  var counts = {}
  while console.read_line is Some(line)
    key = if options.ignore_case?: line.ascii_lowercase else: line
    previous = counts.get(key) or CountedLine(count: 0, line: line)
    counts = counts.set(key, CountedLine(count: previous.count + 1, line: previous.line))
  end

  ranked = counts.values.sort_by_desc(fn(item) item.count end)
  selected = ranked.take(options.top or ranked.size)
  width = selected.first.map(fn(item) item.count.to_text.char_count end) or 0
  for item in selected
    console.print("#{item.count.to_text.pad_left(width)} #{item.line}")
  end
  0
end

test "counts are descending and ties retain first appearance"
  console = Console.fake_input(["z", "a", "b", "a", "b", "b", "z"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["3 b", "2 z", "2 a"]
  assert console.printed_errors == []
end

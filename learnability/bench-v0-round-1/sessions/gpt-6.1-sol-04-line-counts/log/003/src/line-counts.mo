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
    # A limit larger than any list size still means print every available line.
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

test "empty input produces no output"
  console = Console.fake_input([])
  assert main(Args.of([]), console) == 0
  assert console.printed == []
  assert console.printed_errors == []
end

test "empty lines and spaces are significant and case is exact by default"
  console = Console.fake_input(["", " x", "x ", "x", "X", "", " x", "\t", "\r"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["2 ", "2  x", "1 x ", "1 x", "1 X", "1 \t", "1 \r"]
end

test "ASCII case folding preserves first spelling and leaves non-ASCII case alone"
  console = Console.fake_input(["Error", "ERROR", "error", "É", "é", "É", " Error"])
  assert main(Args.of(["--ignore-case"]), console) == 0
  assert console.printed == ["3 Error", "2 É", "1 é", "1  Error"]
end

test "both option orders limit the stable case-folded ranking"
  for arguments in [["--ignore-case", "--top", "2"], ["--top", "2", "--ignore-case"]]
    console = Console.fake_input(["zed", "A", "b", "B", "a", "last", "LAST", "last"])
    assert main(Args.of(arguments), console) == 0
    assert console.printed == ["3 last", "2 A"]
    assert console.printed_errors == []
  end
end

test "counts are right-aligned using the largest printed count"
  console = Console.fake_input(["low"].concat("high\n".repeat(12).lines))
  assert main(Args.of([]), console) == 0
  assert console.printed == ["12 high", " 1 low"]
end

test "top accepts leading zeroes and arbitrarily large positive decimal limits"
  for limit in ["0001", "0002", "999999999999999999999999999999999999999"]
    console = Console.fake_input(["one", "one"])
    assert main(Args.of(["--top", limit]), console) == 0
    assert console.printed == ["2 one"]
    assert console.printed_errors == []
  end
end

test "invalid arguments print only usage and return status two"
  for arguments in [
    ["extra"],
    ["--help"],
    ["--"],
    ["--ignore-case=yes"],
    ["--top=2"],
    ["--top"],
    ["--top", "--ignore-case"],
    ["--top", ""],
    ["--top", "0"],
    ["--top", "000"],
    ["--top", "-1"],
    ["--top", "+1"],
    ["--top", "1.0"],
    ["--top", " 1"],
    ["--top", "1 "],
    ["--top", "1e2"],
    ["--top", "١"],
    ["--top", "999999999999999999999x"],
    ["--top", "2", "extra"],
    ["--ignore-case", "--ignore-case"],
    ["--top", "1", "--top", "2"]
  ]
    console = Console.fake_input(["must not be printed"])
    assert main(Args.of(arguments), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: line-counts [--ignore-case] [--top N]"]
  end
end

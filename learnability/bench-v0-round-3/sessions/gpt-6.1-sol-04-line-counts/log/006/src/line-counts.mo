## Counts distinct input lines, preserving first appearance for ties.
module LineCounts

struct Options
  ignore_case?: Bool
  top: Option(Int)
end

fn parse_top(text: Text) : Option(Int)
  return None if text.empty?
  var number = 0
  for character in text.chars
    return None if not character.ascii_digit?
    # Larger limits are equivalent: a list's size cannot exceed Int's range.
    number = number.saturating_mul(10).saturating_add(character.code - '0'.code)
  end
  if number >= 1: Some(number) else: None
end

fn parse_options(arguments: List(Text)) : Option(Options)
  var options = Options(ignore_case?: false, top: None)
  var remaining = arguments
  while remaining is [argument, ..rest]
    case argument
      "--ignore-case":
        return None if options.ignore_case?
        options.ignore_case? = true
        remaining = rest
      "--top":
        return None if options.top is Some(_)
        text = try rest.first
        limit = try parse_top(text)
        options.top = Some(limit)
        remaining = rest.drop(1)
      _: return None
    end
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
    previous = counts.get(key) or (0, line)
    counts = counts.set(key, (previous.0 + 1, previous.1))
  end

  ranked = counts.values.sort_by_desc(fn(entry) entry.0 end).take(options.top or counts.size)
  width = case ranked.first
    Some(entry): entry.0.to_text.char_count
    None: 0
  end
  for (count, line) in ranked
    console.print("#{count.to_text.pad_left(width)} #{line}")
  end
  0
end

test "frequency sorting keeps first appearance for ties and retains spaces"
  console = Console.fake_input(["z", "a", "a", "z", "", " ", ""])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["2 z", "2 a", "2 ", "1  "]
  assert console.printed_errors == []
end

test "ASCII folding preserves the first spelling and options work in either order"
  for arguments in [["--ignore-case", "--top", "2"], ["--top", "2", "--ignore-case"]]
    console = Console.fake_input(["z", "Error", "É", "ERROR", "é", "Other", "OTHER", "É"])
    assert main(Args.of(arguments), console) == 0
    assert console.printed == ["2 Error", "2 É"]
    assert console.printed_errors == []
  end
  console = Console.fake_input(["Error", "ERROR", "É", "é", "ERROR"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["2 ERROR", "1 Error", "1 É", "1 é"]
end

test "counts align and a top limit larger than the result keeps every line"
  console = Console.fake_input(["single"].concat("many\n".repeat(12).lines))
  assert main(Args.of(["--top", "0003"]), console) == 0
  assert console.printed == ["12 many", " 1 single"]
  assert console.printed_errors == []
end

test "empty input produces no output"
  console = Console.fake_input([])
  assert main(Args.of(["--ignore-case", "--top", "1"]), console) == 0
  assert console.printed == []
  assert console.printed_errors == []
end

test "invalid arguments produce only usage and status two"
  for arguments in [
    ["file"],
    ["--help"],
    ["--"],
    ["--top=2"],
    ["--top"],
    ["--top", ""],
    ["--top", "0"],
    ["--top", "000"],
    ["--top", "-1"],
    ["--top", "+1"],
    ["--top", "1.0"],
    ["--top", " 1"],
    ["--top", "1 "],
    ["--top", "1x"],
    ["--top", "１"],
    ["--top", "--ignore-case"],
    ["--top", "1", "extra"],
    ["--ignore-case", "extra"],
    ["--ignore-case", "--ignore-case"],
    ["--top", "1", "--top", "2"]
  ]
    console = Console.fake_input(["must not print"])
    assert main(Args.of(arguments), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: line-counts [--ignore-case] [--top N]"]
  end
end

test "a whole top limit need not fit an integer"
  console = Console.fake_input(["b", "a", "b"])
  assert main(Args.of(["--top", "99999999999999999999999999999999999"]), console) == 0
  assert console.printed == ["2 b", "1 a"]
  assert console.printed_errors == []
end

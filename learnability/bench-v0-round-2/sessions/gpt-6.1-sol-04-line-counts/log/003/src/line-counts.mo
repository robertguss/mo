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
    # A larger limit already includes every line a list can hold.
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

test "counts are left padded while line whitespace and empty lines stay exact"
  console = Console.fake_input("x\n".repeat(12).lines.concat(["", " x ", "", "x "]))
  assert main(Args.of([]), console) == 0
  assert console.printed == ["12 x", " 2 ", " 1  x ", " 1 x "]
  assert console.printed_errors == []
end

test "case comparison is exact by default and folds only ASCII when requested"
  exact = Console.fake_input(["A", "a", "Ä", "ä", "A"])
  assert main(Args.of([]), exact) == 0
  assert exact.printed == ["2 A", "1 a", "1 Ä", "1 ä"]
  folded = Console.fake_input(["A", "a", "Ä", "ä", "Ä"])
  assert main(Args.of(["--top", "9", "--ignore-case"]), folded) == 0
  assert folded.printed == ["2 A", "2 Ä", "1 ä"]
end

test "empty input produces no output"
  console = Console.fake_input([])
  assert main(Args.of(["--ignore-case", "--top", "1"]), console) == 0
  assert console.printed == []
  assert console.printed_errors == []
end

test "top accepts leading zeroes and numbers beyond the integer range"
  for limit in ["0002", "9223372036854775808", "999999999999999999999999999999999"]
    console = Console.fake_input(["b", "a", "a"])
    assert main(Args.of(["--top", limit]), console) == 0
    assert console.printed == ["2 a", "1 b"]
    assert console.printed_errors == []
  end
end

test "invalid arguments print only usage and return status two"
  for arguments in [
    ["--top"],
    ["--top", "0"],
    ["--top", "000"],
    ["--top", "-1"],
    ["--top", "+1"],
    ["--top", "1.5"],
    ["--top", "1e2"],
    ["--top", " 2"],
    ["--top", "2 "],
    ["--top", ""],
    ["--top", "٢"],
    ["--top", "999999999999999999999999x"],
    ["--top", "--ignore-case"],
    ["--ignore-case", "--top"],
    ["--ignore-case", "--ignore-case"],
    ["--top", "1", "--top", "2"],
    ["--top=2"],
    ["--help"],
    ["file.txt"],
    ["--top", "2", "extra"]
  ]
    console = Console.fake_input(["do not print this"])
    assert main(Args.of(arguments), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: line-counts [--ignore-case] [--top N]"]
  end
end

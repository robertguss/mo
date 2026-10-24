## Counts distinct input lines, preserving first appearance for ties.
module LineCounts

fn main(args: Args, console: Console) : Int
  if not args.list.empty?
    console.error("usage: line-counts [--ignore-case] [--top N]")
    return 2
  end

  var counts = {}
  while console.read_line is Some(line)
    counts = counts.set(line, (counts.get(line) or 0) + 1)
  end

  ranked = counts.entries.sort_by_desc(fn(entry) entry.1 end)
  width = case ranked.first
    Some(entry): entry.1.to_text.char_count
    None: 0
  end
  for (line, count) in ranked
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

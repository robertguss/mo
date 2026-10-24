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

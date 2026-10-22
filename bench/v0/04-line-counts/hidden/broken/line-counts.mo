## Counts how often each line of standard input appears, most frequent first.
module LineCounts

struct Options
  ignore_case?: Bool
  top: Option(Int)
end

## A distinct line: its first spelling, and how often it appeared.
struct Seen
  line: Text
  count: Int
end

fn parse_options(args: List(Text)) : Option(Options)
  examples
    parse_options([]) == Some(Options(ignore_case?: false, top: None))
    parse_options(["--top", "3", "--ignore-case"]) ==
      Some(Options(ignore_case?: true, top: Some(3)))
    parse_options(["--top", "0"]) == None
    parse_options(["--top"]) == None
    parse_options(["-x"]) == None
  end

  var options = Options(ignore_case?: false, top: None)
  var rest = args
  while rest is [flag, ..after]
    rest = after
    if flag == "--ignore-case"
      options.ignore_case? = true
    else if flag == "--top"
      value = try rest.first
      n = try value.to_int
      return None if n < 1
      options.top = Some(n)
      rest = rest.drop(1)
    else
      return None
    end
  end
  Some(options)
end

## The distinct lines, most frequent first, ties in the order they first appeared.
fn count_lines(lines: List(Text), ignore_case?: Bool) : List(Seen)
  examples
    count_lines(["b", "a", "b"], false) == [Seen(line: "b", count: 2), Seen(line: "a", count: 1)]
    count_lines(["A", "a", "b"], true) == [Seen(line: "A", count: 2), Seen(line: "b", count: 1)]
    count_lines(["A", "a"], false) == [Seen(line: "A", count: 1), Seen(line: "a", count: 1)]
  end

  var seen = {}
  for line in lines
    key = if ignore_case?: line.ascii_lowercase else: line
    seen = seen.update(
      key,
      Seen(line: line, count: 0),
      fn(s) Seen(line: line, count: s.count + 1) end
    )
  end
  seen.values.sort_by_desc(fn(s) s.count end)
end

fn main(args: Args, console: Console) : Int
  options = case parse_options(args.list)
    Some(o): o
    None:
      console.error("usage: line-counts [--ignore-case] [--top N]")
      return 2
  end
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  counted = count_lines(lines, options.ignore_case?)
  shown = case options.top
    Some(n): counted.take(n)
    None: counted
  end
  width = "#{shown.first.map(fn(s) s.count end) or 0}".char_count
  for s in shown
    console.print("#{"#{s.count}".pad_left(width)} #{s.line}")
  end
  0
end

test "the most frequent lines come first"
  console = Console.fake_input(["b", "a", "b", "c", "b", "a"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["3 b", "2 a", "1 c"]
end

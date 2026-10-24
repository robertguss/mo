## Reads lines from standard input and prints each distinct line once with how
## many times it appeared, most frequent first.
## Usage: line-counts [--ignore-case] [--top N]
module LineCounts

struct Options
  ignore_case?: Bool
  top: Option(Int)
end

usage = "usage: line-counts [--ignore-case] [--top N]"

## The options on the command line, or None for a usage error.
fn parse_args(args: List(Text)) : Option(Options)
  examples
    parse_args([]) == Some(Options(ignore_case?: false, top: None))
    parse_args(["--top", "2", "--ignore-case"]) == Some(Options(ignore_case?: true, top: Some(2)))
    parse_args(["--ignore-case", "--top", "1"]) == Some(Options(ignore_case?: true, top: Some(1)))
    parse_args(["--top"]) == None
    parse_args(["--top", "0"]) == None
    parse_args(["--top", "-3"]) == None
    parse_args(["--top", "x"]) == None
    parse_args(["--top", "1", "--top", "2"]) == None
    parse_args(["--ignore-case", "--ignore-case"]) == None
    parse_args(["file.txt"]) == None
  end

  parse_from(args, Options(ignore_case?: false, top: None))
end

fn parse_from(args: List(Text), options: Options) : Option(Options)
  case args
    []: Some(options)
    ["--ignore-case", ..rest] if not options.ignore_case?: parse_from(
      rest,
      Options(ignore_case?: true, top: options.top)
    )
    ["--top", value, ..rest] if options.top == None:
      n = try value.to_int
      return None if n < 1
      parse_from(rest, Options(ignore_case?: options.ignore_case?, top: Some(n)))
    [_, .._]: None
  end
end

## Each distinct line with its count, in the order each first appeared.
fn count_lines(lines: List(Text), ignore_case?: Bool) : List((Text, Int))
  examples
    count_lines(["a", "b", "a"], false) == [("a", 2), ("b", 1)]
    count_lines(["Error", "ERROR", "error"], true) == [("Error", 3)]
    count_lines(["Error", "ERROR"], false) == [("Error", 1), ("ERROR", 1)]
    count_lines(["", " ", ""], false) == [("", 2), (" ", 1)]
    count_lines([], false) == []
  end

  var positions = {}
  var counts = []
  for line in lines
    key = if ignore_case?: line.ascii_lowercase else: line
    case positions.get(key)
      Some(i):
        (first, n) = counts.get(i) or stop "every stored position is in counts"
        counts = counts.set(i, (first, n + 1)) or stop "the position is in counts"
      None:
        positions = positions.set(key, counts.size)
        counts = counts.append((line, 1))
    end
  end
  counts
end

## The output lines: most frequent first, ties in first-appearance order,
## at most `top` of them, counts padded to the widest count printed.
fn report(counts: List((Text, Int)), top: Option(Int)) : List(Text)
  examples
    report([("a", 1), ("b", 3)], None) == ["3 b", "1 a"]
    report([("a", 2), ("b", 2), ("c", 3)], Some(2)) == ["3 c", "2 a"]
    report([("x", 12), ("y", 1)], None) == ["12 x", " 1 y"]
    report([("x", 12), ("y", 1), ("z", 1)], Some(5)) == ["12 x", " 1 y", " 1 z"]
    report([], None) == []
  end

  sorted = counts.sort_by_desc(fn(entry) entry.1 end)
  shown = case top
    Some(n): sorted.take(n)
    None: sorted
  end
  width = shown.reduce(0, fn(w, entry) w.max(entry.1.to_text.char_count) end)
  shown.map(fn(entry) "#{entry.1.to_text.pad_left(width)} #{entry.0}" end)
end

fn main(args: Args, console: Console) : Int
  case parse_args(args.list)
    None:
      console.error(usage)
      2
    Some(options):
      var lines = []
      while console.read_line is Some(line)
        lines = lines.append(line)
      end
      for out in report(count_lines(lines, options.ignore_case?), options.top)
        console.print(out)
      end
      0
  end
end

test "the example from the task"
  console = Console.fake_input([
    "GET /",
    "POST /login",
    "GET /",
    "GET /about",
    "POST /login",
    "GET /"
  ])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["3 GET /", "2 POST /login", "1 GET /about"]
  assert console.printed_errors == []
end

test "ignore case and top together"
  console = Console.fake_input(["b", "Error", "ERROR", "b", "error", "c"])
  assert main(Args.of(["--top", "2", "--ignore-case"]), console) == 0
  assert console.printed == ["3 Error", "2 b"]
end

test "a usage error prints only the usage line"
  console = Console.fake_input(["a"])
  assert main(Args.of(["--top"]), console) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: line-counts [--ignore-case] [--top N]"]
end

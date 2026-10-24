## Reads lines from standard input and prints each distinct line once with how many times it
## appeared, most frequent first. Usage: `line-counts [--ignore-case] [--top N]`.
module LineCounts

usage = "usage: line-counts [--ignore-case] [--top N]"

## What the command line asks for.
struct Options
  ignore_case?: Bool
  top: Option(Int)
end

## Reads the command line, or gives `None` when it is not a valid one.
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
    parse_args(["extra"]) == None
  end

  case args
    []: Some(Options(ignore_case?: false, top: None))
    ["--ignore-case", ..rest]:
      options = try parse_args(rest)
      return None if options.ignore_case?
      Some(Options(ignore_case?: true, top: options.top))
    ["--top", n, ..rest]:
      top = try n.to_int
      return None if top < 1
      options = try parse_args(rest)
      return None if options.top != None
      Some(Options(ignore_case?: options.ignore_case?, top: Some(top)))
    [_, .._]: None
  end
end

## Each distinct line, as first spelled, with its count, most frequent first; lines with equal
## counts keep the order in which they first appeared.
fn count_lines(lines: List(Text), ignore_case?: Bool) : List((Text, Int))
  examples
    count_lines(["b", "a", "b", "c", "a", "b"], false) == [("b", 3), ("a", 2), ("c", 1)]
    count_lines(["x", "y", "z"], false) == [("x", 1), ("y", 1), ("z", 1)]
    count_lines(["Error", "ERROR", "error "], true) == [("Error", 2), ("error ", 1)]
    count_lines(["Error", "ERROR"], false) == [("Error", 1), ("ERROR", 1)]
    count_lines(["", " ", ""], false) == [("", 2), (" ", 1)]
    count_lines([], false) == []
  end

  var seen = {}
  for line in lines
    key = if ignore_case?: line.ascii_lowercase else: line
    entry = case seen.get(key)
      Some((first, n)): (first, n + 1)
      None: (line, 1)
    end
    seen = seen.set(key, entry)
  end
  seen.values.sort_by_desc(fn(entry) entry.1 end)
end

## The output lines: each count padded to the width of the largest count printed.
fn format(counts: List((Text, Int))) : List(Text)
  examples
    format([("a", 10), ("b", 9)]) == ["10 a", " 9 b"]
    format([("", 1)]) == ["1 "]
    format([]) == []
  end

  width = counts.reduce(0, fn(widest, entry) widest.max("#{entry.1}".char_count) end)
  counts.map(fn(entry) "#{"#{entry.1}".pad_left(width)} #{entry.0}" end)
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
      counts = count_lines(lines, options.ignore_case?)
      shown = case options.top
        Some(n): counts.take(n)
        None: counts
      end
      for out in format(shown)
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

test "the width comes from the largest count printed, after --top"
  var input = ["b"]
  for _ in 0..10
    input = input.append("a")
  end
  input = input.append("c")
  console = Console.fake_input(input)
  assert main(Args.of(["--top", "2"]), console) == 0
  assert console.printed == ["10 a", " 1 b"]
  shorter = Console.fake_input(input)
  assert main(Args.of(["--top", "5"]), shorter) == 0
  assert shorter.printed == ["10 a", " 1 b", " 1 c"]
end

test "ignore-case keeps the first spelling, and options go in either order"
  console = Console.fake_input(["error", "Error", "ERROR", "warn", "Warn"])
  assert main(Args.of(["--top", "1", "--ignore-case"]), console) == 0
  assert console.printed == ["3 error"]
end

test "a bad command line prints only the usage, with status 2"
  for bad in [["--top"], ["--top", "0"], ["--top", "two"], ["-x"], ["--ignore-case", "file"]]
    console = Console.fake_input(["a"])
    assert main(Args.of(bad), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: line-counts [--ignore-case] [--top N]"]
  end
end

test "no input prints nothing"
  console = Console.fake_input([])
  assert main(Args.of([]), console) == 0
  assert console.printed == []
end

## Reads lines from standard input and prints each distinct line once with how many
## times it appeared, most frequent first. Usage: line-counts [--ignore-case] [--top N]
module LineCounts

usage = "usage: line-counts [--ignore-case] [--top N]"

## The choices made on the command line.
struct Options
  ignore_case?: Bool
  top: Option(Int)
end

## A distinct line: how often it appeared, and its spelling the first time.
struct Tally
  count: Int
  line: Text
end

## Reads the command line, or gives None for a usage error.
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

  var options = Options(ignore_case?: false, top: None)
  var rest = args
  while rest is [arg, ..after]
    case arg
      "--ignore-case":
        return None if options.ignore_case?
        options.ignore_case? = true
        rest = after
      "--top":
        return None if options.top != None
        case after
          [n_text, ..remaining]:
            n = n_text.to_int or 0
            return None if n < 1
            options.top = Some(n)
            rest = remaining
          []: return None
        end
      _: return None
    end
  end
  Some(options)
end

## Counts each distinct line, keeping the order in which each first appeared.
fn tally(lines: List(Text), ignore_case?: Bool) : List(Tally)
  examples
    tally(["a", "b", "a"], false) == [Tally(count: 2, line: "a"), Tally(count: 1, line: "b")]
    tally(["Error", "ERROR", "error"], true) == [Tally(count: 3, line: "Error")]
    tally(["Error", "ERROR"], false) == [
      Tally(count: 1, line: "Error"),
      Tally(count: 1, line: "ERROR")
    ]
    tally([], false) == []
  end

  var seen = {}
  for line in lines
    key = if ignore_case?: line.ascii_lowercase else: line
    current = seen.get(key) or Tally(count: 0, line: line)
    seen = seen.set(key, Tally(count: current.count + 1, line: current.line))
  end
  seen.values
end

## Formats the tallies most frequent first, keeping first-appearance order for ties.
fn report(tallies: List(Tally), top: Option(Int)) : List(Text)
  examples
    report([Tally(count: 1, line: "a"), Tally(count: 12, line: "b")], None) == ["12 b", " 1 a"]
    report([Tally(count: 1, line: "a"), Tally(count: 12, line: "b")], Some(1)) == ["12 b"]
    report(
      [Tally(count: 9, line: "a"), Tally(count: 12, line: "b"), Tally(count: 9, line: "")],
      Some(2)
    ) == ["12 b", " 9 a"]
    report([Tally(count: 2, line: "x")], Some(5)) == ["2 x"]
    report([], None) == []
  end

  sorted = tallies.sort_by_desc(fn(t) t.count end)
  shown = case top
    Some(n): sorted.take(n)
    None: sorted
  end
  width = shown.reduce(0, fn(widest, t) widest.max(t.count.to_text.char_count) end)
  shown.map(fn(t) "#{t.count.to_text.pad_left(width)} #{t.line}" end)
end

fn main(args: Args, console: Console) : Int
  case parse_args(args.list)
    Some(options):
      var lines = []
      while console.read_line is Some(line)
        lines = lines.append(line)
      end
      for out in report(tally(lines, options.ignore_case?), options.top)
        console.print(out)
      end
      0
    None:
      console.error(usage)
      2
  end
end

test "the example from the task"
  input = ["GET /", "POST /login", "GET /", "GET /about", "POST /login", "GET /"]
  console = Console.fake_input(input)
  assert main(Args.of([]), console) == 0
  assert console.printed == ["3 GET /", "2 POST /login", "1 GET /about"]
  assert console.printed_errors == []
end

test "ignore case keeps the first spelling, and top limits the output"
  input = ["b", "Error", "ERROR", " error", "", "", "error"]
  console = Console.fake_input(input)
  assert main(Args.of(["--top", "2", "--ignore-case"]), console) == 0
  assert console.printed == ["3 Error", "2 "]
end

test "a usage error prints only the usage line"
  console = Console.fake_input(["a"])
  assert main(Args.of(["--top"]), console) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: line-counts [--ignore-case] [--top N]"]
end

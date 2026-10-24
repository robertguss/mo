## Merges inclusive whole-number ranges, or prints the gaps between them.
module Ranges

struct Span
  low: Int
  high: Int
end

fn trim_edges(text: Text) : Text
  chars = text.chars
  var start = 0
  var finish = chars.size
  while chars.get(start) is Some(c) and (c == ' ' or c == '\t')
    start += 1
  end
  while finish > start
    c = chars.get(finish - 1) or ' '
    break if c != ' ' and c != '\t'
    finish -= 1
  end
  Text.from_chars(chars.drop(start).take(finish - start))
end

fn number(text: Text) : Option(Int)
  return None if text.empty?
  return None if not text.chars.all?(fn(c) c.ascii_digit? end)
  text.to_int
end

fn parse_span(text: Text) : Option(Span)
  case text.split("-")
    [single]:
      n = try number(single)
      Some(Span(low: n, high: n))
    [left, right]:
      low = try number(left)
      high = try number(right)
      if low <= high: Some(Span(low: low, high: high)) else: None
    _: None
  end
end

fn print_span(span: Span, console: Console)
  console.print(if span.low == span.high: "#{span.low}" else: "#{span.low}-#{span.high}")
end

fn main(args: Args, console: Console) : Int
  if not args.list.empty? and args.list != ["--gaps"]
    console.error("usage: ranges [--gaps]")
    return 2
  end
  gaps? = args.list == ["--gaps"]
  var spans = []
  var status = 0
  var line_number = 0
  while console.read_line is Some(line)
    line_number += 1
    text = trim_edges(line)
    continue if text.empty?
    case parse_span(text)
      Some(span): spans = spans.append(span)
      None:
        console.error("line #{line_number}: not a range: #{text}")
        status = 1
    end
  end
  sorted = spans.sort
  var current = case sorted.first
    Some(first): first
    None: return status
  end
  for next in sorted.drop(1)
    if next.low <= current.high.saturating_add(1)
      current.high = current.high.max(next.high)
    else
      if gaps?
        print_span(Span(low: current.high + 1, high: next.low - 1), console)
      else
        print_span(current, console)
      end
      current = next
    end
  end
  if not gaps?
    print_span(current, console)
  end
  status
end

test "unsorted overlapping and touching ranges merge"
  console = Console.fake_input(["10-12", "3-5", "6-8", "15", "11-14"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["3-8", "10-15"]
  assert console.printed_errors == []
end

test "gaps include only uncovered numbers between merged ranges"
  console = Console.fake_input(["10-12", "3-5", "6-8", "15", "11-14"])
  assert main(Args.of(["--gaps"]), console) == 0
  assert console.printed == ["9"]
  assert console.printed_errors == []
end

test "invalid lines are numbered and skipped while valid ranges still merge"
  console = Console.fake_input([
    "",
    " \t",
    " \t0002-0004\t ",
    " 5-3 \t",
    "1 -2",
    "-1",
    "+2",
    "1-",
    "1--2",
    "1-2-3",
    "9223372036854775808",
    "\u{FF11}",
    "\r5",
    "6\r",
    "\u{B}7",
    "8\u{C}",
    "9\u{A0}",
    "0005",
    "\tbad text\t"
  ])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["2-5"]
  assert console.printed_errors == [
    "line 4: not a range: 5-3",
    "line 5: not a range: 1 -2",
    "line 6: not a range: -1",
    "line 7: not a range: +2",
    "line 8: not a range: 1-",
    "line 9: not a range: 1--2",
    "line 10: not a range: 1-2-3",
    "line 11: not a range: 9223372036854775808",
    "line 12: not a range: \u{FF11}",
    "line 13: not a range: \r5",
    "line 14: not a range: 6\r",
    "line 15: not a range: \u{B}7",
    "line 16: not a range: 8\u{C}",
    "line 17: not a range: 9\u{A0}",
    "line 19: not a range: bad text"
  ]
end

test "duplicates containment and adjacency at the largest integer are safe"
  console = Console.fake_input([
    "9223372036854775807",
    "0-0",
    "9223372036854775805-9223372036854775806",
    "9223372036854775807",
    "2-9",
    "3-4",
    "2-7",
    "000000000000000000000000000000001"
  ])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["0-9", "9223372036854775805-9223372036854775807"]
  assert console.printed_errors == []
end

test "gaps exclude both outside tails and preserve multiple gaps"
  console = Console.fake_input(["9223372036854775807", "0", "4-6", "2"])
  assert main(Args.of(["--gaps"]), console) == 0
  assert console.printed == ["1", "3", "7-9223372036854775806"]
  assert console.printed_errors == []
end

test "empty input and a single merged range have no gaps"
  for arguments in [[], ["--gaps"]]
    console = Console.fake_input(["", " \t "])
    assert main(Args.of(arguments), console) == 0
    assert console.printed == []
    assert console.printed_errors == []
  end
  console = Console.fake_input(["0-9223372036854775807", "2", "9223372036854775807"])
  assert main(Args.of(["--gaps"]), console) == 0
  assert console.printed == []
  assert console.printed_errors == []
end

test "usage errors produce only usage and do not read input"
  for arguments in [["--help"], [""], ["--gaps", "--gaps"], ["--gaps", "extra"], ["--"]]
    console = Console.fake_input(["invalid", "1-3"])
    assert main(Args.of(arguments), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: ranges [--gaps]"]
    assert console.read_line == Some("invalid")
  end
end

test "gaps still print when another input line is invalid"
  console = Console.fake_input(["2", "\tno\t", "6"])
  assert main(Args.of(["--gaps"]), console) == 1
  assert console.printed == ["3-5"]
  assert console.printed_errors == ["line 2: not a range: no"]
end

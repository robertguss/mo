## Merges whole-number ranges, or prints the gaps between them.
module Ranges

struct Span
  low: Int
  high: Int
end

fn trim_line(text: Text) : Text
  chars = text.chars
  var start = 0
  var finish = chars.size
  while chars.get(start) == Some(' ') or chars.get(start) == Some('\t')
    start += 1
  end
  while finish > start and (
    chars.get(finish - 1) == Some(' ') or chars.get(finish - 1) == Some('\t')
  )
    finish -= 1
  end
  Text.from_chars(chars.take(finish).drop(start))
end

fn parse_number(text: Text) : Option(Int)
  return None if text.empty? or not text.chars.all?(fn(c) c.ascii_digit? end)
  text.to_int
end

fn parse_range(text: Text) : Option(Span)
  case text.split("-")
    [single]:
      number = try parse_number(single)
      Some(Span(low: number, high: number))
    [low_text, high_text]:
      low = try parse_number(low_text)
      high = try parse_number(high_text)
      if low <= high: Some(Span(low: low, high: high)) else: None
    _: None
  end
end

fn print_span(span: Span, console: Console)
  if span.low == span.high
    console.print("#{span.low}")
  else
    console.print("#{span.low}-#{span.high}")
  end
end

fn main(args: Args, console: Console) : Int
  if args.list != [] and args.list != ["--gaps"]
    console.error("usage: ranges [--gaps]")
    return 2
  end
  gaps? = args.list == ["--gaps"]
  var spans = []
  var line_number = 0
  var status = 0
  while console.read_line is Some(raw)
    line_number += 1
    text = trim_line(raw)
    continue if text.empty?
    case parse_range(text)
      Some(span): spans = spans.append(span)
      None:
        console.error("line #{line_number}: not a range: #{text}")
        status = 1
    end
  end

  var current = None
  for span in spans.sort
    case current
      None: current = Some(span)
      Some(previous):
        if span.low <= previous.high.saturating_add(1)
          current = Some(Span(low: previous.low, high: previous.high.max(span.high)))
        else
          if gaps?
            print_span(Span(low: previous.high + 1, high: span.low - 1), console)
          else
            print_span(previous, console)
          end
          current = Some(span)
        end
    end
  end
  if not gaps? and current is Some(last)
    print_span(last, console)
  end
  status
end

test "overlap and touching ranges merge, with only internal gaps printed"
  lines = ["10-12", "3-5", "6-8", "15", "11-14"]
  console = Console.fake_input(lines)
  assert main(Args.of([]), console) == 0
  assert console.printed == ["3-8", "10-15"]
  assert console.printed_errors == []
  gaps = Console.fake_input(lines)
  assert main(Args.of(["--gaps"]), gaps) == 0
  assert gaps.printed == ["9"]
  assert gaps.printed_errors == []
end

test "invalid lines are diagnosed in order while valid ranges still contribute"
  lines = [
    "",
    " \t ",
    " 001-003 \t",
    " 5-4 ",
    "-1",
    "1-",
    "1-2-3",
    "1 -2",
    "+1",
    "١",
    "9223372036854775808",
    "\u{000B}2",
    "2\r",
    "4",
    "\tbad\t "
  ]
  for arguments in [[], ["--gaps"]]
    console = Console.fake_input(lines)
    assert main(Args.of(arguments), console) == 1
    assert console.printed == (if arguments.empty?: ["1-4"] else: [])
    assert console.printed_errors == [
      "line 4: not a range: 5-4",
      "line 5: not a range: -1",
      "line 6: not a range: 1-",
      "line 7: not a range: 1-2-3",
      "line 8: not a range: 1 -2",
      "line 9: not a range: +1",
      "line 10: not a range: ١",
      "line 11: not a range: 9223372036854775808",
      "line 12: not a range: \u{000B}2",
      "line 13: not a range: 2\r",
      "line 15: not a range: bad"
    ]
  end
end

test "duplicates containment leading zeros and integer boundaries preserve exact coverage"
  lines = [
    "9223372036854775807",
    "9223372036854775805-9223372036854775806",
    "9223372036854775807",
    "0-0",
    "0".repeat(80),
    "20-22",
    "00021",
    "18-20",
    "4-5",
    "4-4",
    "3-6"
  ]
  console = Console.fake_input(lines)
  assert main(Args.of([]), console) == 0
  assert console.printed == ["0", "3-6", "18-22", "9223372036854775805-9223372036854775807"]
  assert console.printed_errors == []
  gaps = Console.fake_input(lines)
  assert main(Args.of(["--gaps"]), gaps) == 0
  assert gaps.printed == ["1-2", "7-17", "23-9223372036854775804"]
  assert gaps.printed_errors == []
end

test "empty input prints nothing and a full-domain range has no gaps"
  for lines in [[], ["", " \t"]]
    for arguments in [[], ["--gaps"]]
      console = Console.fake_input(lines)
      assert main(Args.of(arguments), console) == 0
      assert console.printed == []
      assert console.printed_errors == []
    end
  end
  console = Console.fake_input(["0-9223372036854775807", "1", "9223372036854775807"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["0-9223372036854775807"]
  gaps = Console.fake_input(["0-9223372036854775807"])
  assert main(Args.of(["--gaps"]), gaps) == 0
  assert gaps.printed == []
end

test "usage errors take precedence over input and print only usage"
  for arguments in [
    ["--gaps", "--gaps"],
    ["--GAPS"],
    ["--gaps=1"],
    ["--"],
    ["file"],
    ["--gaps", "file"]
  ]
    console = Console.fake_input(["bad", "1-5"])
    assert main(Args.of(arguments), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: ranges [--gaps]"]
  end
end

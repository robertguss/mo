## Reads ranges of whole numbers from standard input, one on each line, and
## prints the fewest ranges that cover the same numbers, or with `--gaps` the
## ranges of numbers between them.
module Ranges

## The numbers from `low` to `high`, both included.
struct Span
  low: Int
  high: Int
end

## Whether a character is a space or a tab.
fn blank?(c: Char) : Bool
  c == ' ' or c == '\t'
end

## The text without the spaces and tabs at its ends.
fn trim_blanks(text: Text) : Text
  examples
    trim_blanks(" \t3-5 \t") == "3-5"
    trim_blanks("\r 7\r") == "\r 7\r"
    trim_blanks(" \t ") == ""
  end

  chars = text.chars
  var start = 0
  while start < chars.size and chars.get(start).map(blank?) == Some(true)
    start += 1
  end
  var finish = chars.size
  while finish > start and chars.get(finish - 1).map(blank?) == Some(true)
    finish -= 1
  end
  Text.from_chars(chars.drop(start).take(finish - start))
end

## A number: one or more ASCII digits that fit in an `Int`.
fn parse_number(text: Text) : Option(Int)
  examples
    parse_number("007") == Some(7)
    parse_number("9223372036854775807") == Some(9223372036854775807)
    parse_number("9223372036854775808") == None
    parse_number("-1") == None
    parse_number("+1") == None
    parse_number("") == None
    parse_number("1 ") == None
  end

  return None if text.empty?
  return None if not text.chars.all?(fn(c) c.ascii_digit? end)
  text.to_int
end

## A range, `LOW-HIGH` or `N`, from a line already trimmed.
fn parse_span(text: Text) : Option(Span)
  examples
    parse_span("3-5") == Some(Span(low: 3, high: 5))
    parse_span("15") == Some(Span(low: 15, high: 15))
    parse_span("5-5") == Some(Span(low: 5, high: 5))
    parse_span("5-3") == None
    parse_span("1-2-3") == None
    parse_span("-3") == None
    parse_span("3-") == None
    parse_span("3 - 5") == None
    parse_span("abc") == None
  end

  case text.split("-")
    [single]:
      n = try parse_number(single)
      Some(Span(low: n, high: n))
    [first, second]:
      low = try parse_number(first)
      high = try parse_number(second)
      if low <= high: Some(Span(low: low, high: high)) else: None
    _: None
  end
end

## The fewest spans covering the same numbers, in increasing order.
fn merge(spans: List(Span)) : List(Span)
  examples
    merge([]) == []
    merge([Span(low: 6, high: 8), Span(low: 3, high: 5)]) == [Span(low: 3, high: 8)]
    merge([Span(low: 1, high: 10), Span(low: 2, high: 3)]) == [Span(low: 1, high: 10)]
    merge([Span(low: 5, high: 6), Span(low: 1, high: 3)]) == [
      Span(low: 1, high: 3),
      Span(low: 5, high: 6)
    ]
    merge([Span(low: 0, high: 9223372036854775807), Span(low: 4, high: 4)]) == [
      Span(low: 0, high: 9223372036854775807)
    ]
  end

  var merged = []
  var current = None
  for span in spans.sort_by(fn(s) s.low end)
    case current
      None: current = Some(span)
      Some(open):
        if span.low - 1 <= open.high
          current = Some(Span(low: open.low, high: open.high.max(span.high)))
        else
          merged = merged.append(open)
          current = Some(span)
        end
    end
  end
  case current
    Some(open): merged.append(open)
    None: merged
  end
end

## The spans of numbers between merged spans, which are in none of them.
fn gaps(merged: List(Span)) : List(Span)
  examples
    gaps([]) == []
    gaps([Span(low: 3, high: 8), Span(low: 10, high: 15)]) == [Span(low: 9, high: 9)]
    gaps([Span(low: 0, high: 1), Span(low: 5, high: 6), Span(low: 9, high: 9)]) == [
      Span(low: 2, high: 4),
      Span(low: 7, high: 8)
    ]
  end

  merged.zip(merged.drop(1)).map(fn(pair) Span(low: pair.0.high + 1, high: pair.1.low - 1) end)
end

## A span written `LOW-HIGH`, or `N` for a span of one number.
fn to_text(span: Span) : Text
  examples
    Span(low: 3, high: 8).to_text == "3-8"
    Span(low: 9, high: 9).to_text == "9"
  end

  if span.low == span.high: "#{span.low}" else: "#{span.low}-#{span.high}"
end

fn main(args: Args, console: Console) : Int
  show_gaps? = case args.list
    []: false
    ["--gaps"]: true
    _:
      console.error("usage: ranges [--gaps]")
      return 2
  end
  var spans = []
  var status = 0
  var number = 0
  while console.read_line is Some(line)
    number += 1
    trimmed = trim_blanks(line)
    continue if trimmed.empty?
    case parse_span(trimmed)
      Some(span): spans = spans.append(span)
      None:
        console.error("line #{number}: not a range: #{trimmed}")
        status = 1
    end
  end
  merged = merge(spans)
  output = if show_gaps?: gaps(merged) else: merged
  for span in output
    console.print(span.to_text)
  end
  status
end

test "the example merges into two ranges"
  console = Console.fake_input(["10-12", "3-5", "6-8", "15", "11-14"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["3-8", "10-15"]
  assert console.printed_errors == []
end

test "the example's gap is one number"
  console = Console.fake_input(["10-12", "3-5", "6-8", "15", "11-14"])
  assert main(Args.of(["--gaps"]), console) == 0
  assert console.printed == ["9"]
end

test "bad lines are reported and skipped, blank lines are counted"
  console = Console.fake_input([" 1-2\t", "", "  ", "x", " 5-3 ", "4"])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["1-2", "4"]
  assert console.printed_errors == ["line 4: not a range: x", "line 5: not a range: 5-3"]
end

test "other arguments are a usage error"
  console = Console.fake_input(["1-2"])
  assert main(Args.of(["--gap"]), console) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: ranges [--gaps]"]
  other = Console.fake_input(["1-2"])
  assert main(Args.of(["--gaps", "--gaps"]), other) == 2
  assert other.printed == []
end

test "no input prints nothing"
  console = Console.fake_input([])
  assert main(Args.of(["--gaps"]), console) == 0
  assert console.printed == []
end

## Reads ranges of whole numbers, one on each line of standard input, and prints
## the fewest ranges that cover the same numbers, or with `--gaps` the ranges
## of numbers that lie between them.
module Ranges

## The whole numbers from `low` to `high`, both included.
struct Span
  low: Int
  high: Int
end

## A span's text form: `LOW-HIGH`, or `N` for a span of one number.
fn to_text(span: Span) : Text
  if span.low == span.high: "#{span.low}" else: "#{span.low}-#{span.high}"
end

fn valid?(span: Span) : Bool
  span.low <= span.high
end

fn blank?(c: Char) : Bool
  c == ' ' or c == '\t'
end

## The line without the spaces and tabs at its ends.
fn strip(line: Text) : Text
  examples
    strip(" \t3-5\t ") == "3-5"
    strip("\t \t") == ""
    strip("a b") == "a b"
    strip("\u{B}7\u{B}") == "\u{B}7\u{B}"
  end

  chars = line.chars
  var start = 0
  var finish = chars.size
  while start < finish and chars.get(start).map(blank?) == Some(true)
    start += 1
  end
  while finish > start and chars.get(finish - 1).map(blank?) == Some(true)
    finish -= 1
  end
  Text.from_chars(chars.drop(start).take(finish - start))
end

## One or more ASCII digits that fit in an `Int`.
fn parse_number(text: Text) : Option(Int)
  examples
    parse_number("007") == Some(7)
    parse_number("9223372036854775807") == Some(9223372036854775807)
    parse_number("9223372036854775808") == None
    parse_number("") == None
    parse_number("-3") == None
    parse_number("+3") == None
    parse_number("1_0") == None
  end

  return None if text.empty? or not text.chars.all?(fn(c) c.ascii_digit? end)
  text.to_int
end

## A stripped line as a span: `LOW-HIGH` with LOW not above HIGH, or `N`.
fn parse_span(text: Text) : Option(Span)
  examples
    parse_span("3-5") == Some(Span(low: 3, high: 5))
    parse_span("15") == Some(Span(low: 15, high: 15))
    parse_span("5-5") == Some(Span(low: 5, high: 5))
    parse_span("5-3") == None
    parse_span("3 - 5") == None
    parse_span("3-") == None
    parse_span("-3") == None
    parse_span("1-2-3") == None
  end

  case text.split("-")
    [single]: parse_number(single).map(fn(n) Span(low: n, high: n) end)
    [first, second]:
      low = try parse_number(first)
      high = try parse_number(second)
      if low <= high: Some(Span(low: low, high: high)) else: None
    _: None
  end
end

## The fewest spans, in increasing order, that cover the same numbers.
fn merge(spans: List(Span)) : List(Span)
  promise "the result is in increasing order, with a gap between neighbours, and covers each span"
    spans.all?(valid?) implies result.all?(valid?)
    spans.all?(valid?) implies
      result.zip(result.drop(1)).all?(fn(pair) pair.0.high.saturating_add(1) < pair.1.low end)
    spans.all?(valid?) implies
      spans.all?(fn(s) result.any?(fn(r) r.low <= s.low and s.high <= r.high end) end)
    spans.all?(valid?) implies
      result.all?(fn(r)
        spans.any?(fn(s) s.low == r.low end) and spans.any?(fn(s) s.high == r.high end)
      end)
    allows spans: [Span(low: 3, high: 5), Span(low: 6, high: 8)], result: [Span(low: 3, high: 8)]
    forbids spans: [Span(low: 3, high: 5), Span(low: 6, high: 8)], result: [
      Span(low: 3, high: 5),
      Span(low: 6, high: 8)
    ]
  end

  examples
    merge([]) == []
    merge([Span(low: 6, high: 8), Span(low: 3, high: 5)]) == [Span(low: 3, high: 8)]
    merge([Span(low: 1, high: 9), Span(low: 2, high: 3)]) == [Span(low: 1, high: 9)]
    merge([Span(low: 7, high: 7), Span(low: 0, high: 5)]) == [
      Span(low: 0, high: 5),
      Span(low: 7, high: 7)
    ]
  end

  var merged = []
  var current = None
  for span in spans.sort
    current = case current
      None: Some(span)
      Some(open) if span.low <= open.high.saturating_add(1): Some(
        Span(low: open.low, high: open.high.max(span.high))
      )
      Some(open):
        merged = merged.append(open)
        Some(span)
    end
  end
  case current
    Some(open): merged.append(open)
    None: merged
  end
end

## The spans of numbers between merged spans, which lie in none of them.
fn gaps(merged: List(Span)) : List(Span)
  examples
    gaps([]) == []
    gaps([Span(low: 3, high: 8)]) == []
    gaps([Span(low: 3, high: 8), Span(low: 10, high: 15), Span(low: 20, high: 20)]) == [
      Span(low: 9, high: 9),
      Span(low: 16, high: 19)
    ]
  end

  merged.zip(merged.drop(1)).map(fn(pair) Span(low: pair.0.high + 1, high: pair.1.low - 1) end)
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
    text = strip(line)
    continue if text.empty?
    case parse_span(text)
      Some(span): spans = spans.append(span)
      None:
        console.error("line #{number}: not a range: #{text}")
        status = 1
    end
  end
  merged = merge(spans)
  for span in if show_gaps?: gaps(merged) else: merged
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

test "the example has one gap"
  console = Console.fake_input(["10-12", "3-5", "6-8", "15", "11-14"])
  assert main(Args.of(["--gaps"]), console) == 0
  assert console.printed == ["9"]
end

test "bad lines are reported and skipped, and blank lines are counted"
  console = Console.fake_input(["  1-2 ", "", "\t", " 5-3\t", "x", "4", "9223372036854775808"])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["1-2", "4"]
  assert console.printed_errors == [
    "line 4: not a range: 5-3",
    "line 5: not a range: x",
    "line 7: not a range: 9223372036854775808"
  ]
end

test "the largest number merges without overflow"
  console = Console.fake_input(["9223372036854775807", "0-9223372036854775806"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["0-9223372036854775807"]
end

test "anything else on the command line is a usage error"
  for list in [["--gap"], ["--gaps", "--gaps"], ["x"]]
    console = Console.fake_input(["1-2"])
    assert main(Args.of(list), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: ranges [--gaps]"]
  end
end

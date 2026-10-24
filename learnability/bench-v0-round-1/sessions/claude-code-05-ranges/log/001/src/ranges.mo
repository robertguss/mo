## Reads ranges of whole numbers from standard input, one on each line, and prints
## the fewest ranges that cover the same numbers, or with `--gaps` the ranges between them.
module Ranges

## A range of whole numbers, from `low` to `high` with both included.
struct Span
  low: Int
  high: Int
end

## True for a space or a tab, the only characters trimmed from a line's ends.
fn blank?(c: Char) : Bool
  c == ' ' or c == '\t'
end

## The line without the spaces and tabs at its start and end.
fn strip(line: Text) : Text
  examples
    strip("  3-5\t ") == "3-5"
    strip(" \t ") == ""
    strip("a b") == "a b"
    strip("\r7\r") == "\r7\r"
  end

  chars = line.chars
  var start = 0
  while start < chars.size and blank?(chars.get(start) or 'x')
    start += 1
  end
  var finish = chars.size
  while finish > start and blank?(chars.get(finish - 1) or 'x')
    finish -= 1
  end
  Text.from_chars(chars.drop(start).take(finish - start))
end

## A number of one or more ASCII digits that fits in an Int, or None.
fn number(text: Text) : Option(Int)
  examples
    number("42") == Some(42)
    number("007") == Some(7)
    number("") == None
    number("-3") == None
    number("+3") == None
    number("9223372036854775807") == Some(9223372036854775807)
    number("9223372036854775808") == None
    number("00000000000000000000009") == Some(9)
  end

  return None if text.empty?
  return None if not text.chars.all?(fn(c) c.ascii_digit? end)
  digits = text.chars.drop_while_zero
  digits.to_int
end

## The digits without their leading zeros, keeping at least one digit.
fn drop_while_zero(chars: List(Char)) : Text
  var start = 0
  while start < chars.size - 1 and chars.get(start) == Some('0')
    start += 1
  end
  Text.from_chars(chars.drop(start))
end

## The range a trimmed line writes, `LOW-HIGH` or `N`, or None.
fn parse(text: Text) : Option(Span)
  examples
    parse("3-5") == Some(Span(low: 3, high: 5))
    parse("15") == Some(Span(low: 15, high: 15))
    parse("5-5") == Some(Span(low: 5, high: 5))
    parse("5-3") == None
    parse("3-") == None
    parse("-3") == None
    parse("1-2-3") == None
    parse("3 - 5") == None
    parse("x") == None
  end

  parts = text.split("-")
  case parts
    [single]:
      n = try number(single)
      Some(Span(low: n, high: n))
    [first, second]:
      low = try number(first)
      high = try number(second)
      if low <= high: Some(Span(low: low, high: high)) else: None
    _: None
  end
end

## The fewest ranges covering the same numbers, in increasing order.
fn merge(spans: List(Span)) : List(Span)
  examples
    merge([]) == []
    merge([Span(low: 6, high: 8), Span(low: 3, high: 5)]) == [Span(low: 3, high: 8)]
    merge([Span(low: 1, high: 2), Span(low: 4, high: 5)]) == [
      Span(low: 1, high: 2),
      Span(low: 4, high: 5)
    ]
    merge([Span(low: 1, high: 10), Span(low: 2, high: 3)]) == [Span(low: 1, high: 10)]
    merge([Span(low: 0, high: 9223372036854775807), Span(low: 5, high: 6)]) == [
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
    None: merged
    Some(open): merged.append(open)
  end
end

## The ranges of numbers between consecutive merged ranges.
fn gaps(merged: List(Span)) : List(Span)
  examples
    gaps([]) == []
    gaps([Span(low: 3, high: 8)]) == []
    gaps([Span(low: 3, high: 8), Span(low: 10, high: 15)]) == [Span(low: 9, high: 9)]
    gaps([Span(low: 1, high: 2), Span(low: 5, high: 6), Span(low: 9, high: 9)]) == [
      Span(low: 3, high: 4),
      Span(low: 7, high: 8)
    ]
  end

  merged.zip(merged.drop(1)).map(fn(pair) Span(low: pair.0.high + 1, high: pair.1.low - 1) end)
end

## A range written `LOW-HIGH`, or `N` for a range of one number.
fn show(span: Span) : Text
  examples
    show(Span(low: 3, high: 8)) == "3-8"
    show(Span(low: 9, high: 9)) == "9"
  end

  if span.low == span.high: "#{span.low}" else: "#{span.low}-#{span.high}"
end

fn main(args: Args, console: Console) : Int
  gaps? = case args.list
    []: false
    ["--gaps"]: true
    _:
      console.error("usage: ranges [--gaps]")
      return 2
  end
  var spans = []
  var status = 0
  var number_of_line = 0
  while console.read_line is Some(line)
    number_of_line += 1
    text = strip(line)
    continue if text.empty?
    case parse(text)
      Some(span): spans = spans.append(span)
      None:
        console.error("line #{number_of_line}: not a range: #{text}")
        status = 1
    end
  end
  merged = merge(spans)
  output = if gaps?: gaps(merged) else: merged
  for span in output
    console.print(show(span))
  end
  status
end

test "the example merges and finds the gap"
  input = ["10-12", "3-5", "6-8", "15", "11-14"]
  console = Console.fake_input(input)
  assert main(Args.of([]), console) == 0
  assert console.printed == ["3-8", "10-15"]
  gaps_console = Console.fake_input(input)
  assert main(Args.of(["--gaps"]), gaps_console) == 0
  assert gaps_console.printed == ["9"]
end

test "bad lines are reported, blank lines skipped and counted"
  console = Console.fake_input(["  1-3 ", "", "\t", " 9-2\t", "abc", "4"])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["1-4"]
  assert console.printed_errors == ["line 4: not a range: 9-2", "line 5: not a range: abc"]
end

test "other arguments are a usage error"
  console = Console.fake_input(["1-3"])
  assert main(Args.of(["--gap"]), console) == 2
  assert console.printed == []
  assert console.printed_errors == ["usage: ranges [--gaps]"]
  extra = Console.fake_input(["1-3"])
  assert main(Args.of(["--gaps", "x"]), extra) == 2
  assert extra.printed == []
end

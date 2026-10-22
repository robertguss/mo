## Merges the ranges of whole numbers read from standard input.
module Ranges

## The whole numbers from `low` to `high`, both included.
struct Span
  low: Int
  high: Int
end

fn to_text(span: Span) : Text
  if span.low == span.high: "#{span.low}" else: "#{span.low}-#{span.high}"
end

## A number written with ASCII digits only.
fn number(text: Text) : Option(Int)
  examples
    number("42") == Some(42)
    number("-4") == None
    number("") == None
  end

  return None if text.empty? or not text.chars.all?(fn(c) c.ascii_digit? end)
  text.to_int
end

fn parse(text: Text) : Option(Span)
  examples
    parse("3-5") == Some(Span(low: 3, high: 5))
    parse("7") == Some(Span(low: 7, high: 7))
    parse("5-3") == None
    parse("1-2-3") == None
    parse("a") == None
  end

  parts = text.split("-")
  span = case parts
    [single]:
      n = try number(single)
      Span(low: n, high: n)
    [low, high]: Span(low: try number(low), high: try number(high))
    _: return None
  end
  return None if span.low > span.high
  Some(span)
end

## The spans covering the same numbers, in order, with no two overlapping or touching.
fn merge(spans: List(Span)) : List(Span)
  promise "merged spans are in order, and none touches the next"
    result.zip(result.drop(1)).all?(fn(pair) pair.0.high < pair.1.low - 1 end)
    allows spans: [Span(low: 1, high: 2), Span(low: 4, high: 4)], result: [
      Span(low: 1, high: 2),
      Span(low: 4, high: 4)
    ]
    forbids spans: [Span(low: 1, high: 2), Span(low: 3, high: 3)], result: [
      Span(low: 1, high: 2),
      Span(low: 3, high: 3)
    ]
  end
  examples
    merge([Span(low: 6, high: 8), Span(low: 3, high: 5)]) == [Span(low: 3, high: 8)]
    merge([Span(low: 1, high: 10), Span(low: 2, high: 3)]) == [Span(low: 1, high: 10)]
    merge([Span(low: 1, high: 1), Span(low: 3, high: 3)]) == [
      Span(low: 1, high: 1),
      Span(low: 3, high: 3)
    ]
    merge([]) == []
  end

  var merged = []
  for span in spans.sort_by(fn(s) s.low end)
    case merged.last
      Some(last) if span.low - 1 <= last.high: merged = merged.take(merged.size - 1).append(
        Span(low: last.low, high: last.high.max(span.high))
      )
      Some(_) | None: merged = merged.append(span)
    end
  end
  merged
end

## The spans of numbers between merged spans that none of them covers.
fn gaps(merged: List(Span)) : List(Span)
  examples
    gaps([Span(low: 1, high: 2), Span(low: 5, high: 5), Span(low: 7, high: 9)]) == [
      Span(low: 3, high: 4),
      Span(low: 6, high: 6)
    ]
    gaps([Span(low: 1, high: 2)]) == []
  end

  merged.zip(merged.drop(1)).map(fn(pair) Span(low: pair.0.high + 1, high: pair.1.low - 1) end)
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
  var number = 0
  while console.read_line is Some(line)
    number += 1
    trimmed = line.trim
    continue if trimmed.empty?
    case parse(trimmed)
      Some(span): spans = spans.append(span)
      None:
        console.error("line #{number}: not a range: #{trimmed}")
        status = 1
    end
  end
  merged = merge(spans)
  for span in if gaps?: gaps(merged) else: merged
    console.print("#{span}")
  end
  status
end

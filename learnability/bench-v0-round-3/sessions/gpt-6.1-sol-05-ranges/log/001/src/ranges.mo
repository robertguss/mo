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

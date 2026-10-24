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

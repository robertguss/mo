## Merges inclusive whole-number ranges, or prints the gaps between them.
module Ranges

fn trim_edges(text: Text) : Text
  chars = text.chars
  var start = 0
  var finish = chars.size
  while chars.get(start) is Some(c) and (c == ' ' or c == '\t')
    start += 1
  end
  while finish > start
    last = chars.get(finish - 1) or stop "finish is inside the character list"
    break if last != ' ' and last != '\t'
    finish -= 1
  end
  Text.from_chars(chars.drop(start).take(finish - start))
end

fn parse_number(text: Text) : Option(Int)
  return None if text.empty? or not text.chars.all?(fn(c) c.ascii_digit? end)
  text.to_int
end

fn parse_range(text: Text) : Option((Int, Int))
  case text.split("-")
    [single]:
      n = try parse_number(single)
      Some((n, n))
    [low_text, high_text]:
      low = try parse_number(low_text)
      high = try parse_number(high_text)
      if low <= high: Some((low, high)) else: None
    _: None
  end
end

fn merge(items: List((Int, Int))) : List((Int, Int))
  var merged = []
  var current = None
  for item in items.sort
    case current
      None: current = Some(item)
      Some(active):
        if item.0 <= active.1.saturating_add(1)
          current = Some((active.0, active.1.max(item.1)))
        else
          merged = merged.append(active)
          current = Some(item)
        end
    end
  end
  case current
    None: merged
    Some(active): merged.append(active)
  end
end

fn range_text(item: (Int, Int)) : Text
  if item.0 == item.1: "#{item.0}" else: "#{item.0}-#{item.1}"
end

fn main(args: Args, console: Console) : Int
  if args.list != [] and args.list != ["--gaps"]
    console.error("usage: ranges [--gaps]")
    return 2
  end
  gaps? = args.list == ["--gaps"]
  var items = []
  var line_number = 0
  var status = 0
  while console.read_line is Some(line)
    line_number += 1
    trimmed = trim_edges(line)
    continue if trimmed.empty?
    case parse_range(trimmed)
      Some(item): items = items.append(item)
      None:
        console.error("line #{line_number}: not a range: #{trimmed}")
        status = 1
    end
  end
  var previous_high = None
  for item in merge(items)
    if gaps?
      if previous_high is Some(high)
        console.print(range_text((high + 1, item.0 - 1)))
      end
      previous_high = Some(item.1)
    else
      console.print(range_text(item))
    end
  end
  status
end

test "overlapping and touching ranges merge in increasing order"
  console = Console.fake_input(["10-12", "3-5", "6-8", "15", "11-14"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["3-8", "10-15"]
  assert console.printed_errors == []
end

test "gaps are only the uncovered numbers between merged ranges"
  console = Console.fake_input(["10-12", "3-5", "6-8", "15", "11-14"])
  assert main(Args.of(["--gaps"]), console) == 0
  assert console.printed == ["9"]
  assert console.printed_errors == []
end

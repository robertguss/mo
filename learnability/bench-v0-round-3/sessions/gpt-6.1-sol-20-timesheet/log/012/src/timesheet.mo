## Totals the hours each person worked, from timesheet lines read from standard input.
module Timesheet

## One stretch of work, in minutes after midnight.
struct Shift
  name: Text
  start: Int
  finish: Int
end

## Reads a time of day such as `09:30`.
fn parse_time(text: Text) : Option(Int)
  examples
    parse_time("09:30") == Some(570)
    parse_time("24:00") == None
    parse_time("9:30") == None
  end

  case text.split(":")
    [hours, minutes] if hours.char_count == 2 and minutes.char_count == 2:
      h = try hours.to_int
      m = try minutes.to_int
      if h >= 0 and h < 24 and m >= 0 and m < 60: Some(h * 60 + m) else: None
    _: None
  end
end

## Reads a line such as `ada 09:00-12:30`.
fn parse_shift(line: Text) : Option(Shift)
  examples
    parse_shift("ada 09:00-12:30") == Some(Shift(name: "ada", start: 540, finish: 750))
    parse_shift("ada 12:30-09:00") == None
    parse_shift("ada") == None
  end

  case line.words
    [name, span]:
      case span.split("-")
        [from, to]:
          start = try parse_time(from)
          finish = try parse_time(to)
          if start < finish: Some(Shift(name: name, start: start, finish: finish)) else: None
        _: None
      end
    _: None
  end
end

## Writes minutes as `H:MM`.
fn hours(minutes: BigInt) : Text
  examples
    hours(210) == "3:30"
    hours(5) == "0:05"
    hours(1500) == "25:00"
  end

  "#{minutes / 60}:#{"#{minutes % 60}".pad_left(2).replace(" ", "0")}"
end

fn main(args: Args, console: Console) : Int
  over = case args.list
    []: None
    ["--over", text] if not text.empty? and text.chars.all?(fn(c) c.ascii_digit? end):
      var limit = 0.to_big
      for digit in text.chars
        limit = limit * 10 + (digit.code - '0'.code).to_big
      end
      Some(limit * 60)
    _:
      console.error("usage: timesheet [--over HOURS]")
      return 2
  end
  var totals = {}
  var counted = {}
  var status = 0
  var number = 0
  while console.read_line is Some(line)
    number += 1
    continue if line.trim.empty?
    case parse_shift(line)
      Some(shift):
        previous = counted.get(shift.name) or []
        case previous.find(fn(entry)
          shift.start < entry.1.finish and entry.1.start < shift.finish
        end)
          Some(entry):
            console.error("line #{number}: overlaps line #{entry.0}")
            status = 1
          None:
            counted = counted.set(shift.name, previous.append((number, shift)))
            totals = totals.update(shift.name, 0, fn(total) total + shift.finish - shift.start end)
        end
      None:
        console.error("line #{number}: can't read #{line}")
        status = 1
    end
  end
  for name in totals.keys.sort
    total = (totals.get(name) or 0).to_big
    case over
      None: console.print("#{name}: #{hours(total)}")
      Some(limit):
        if total > limit
          console.print("#{name}: #{hours(total)} (#{hours(total - limit)} over)")
        end
    end
  end
  status
end

test "each person's hours are totalled, in order of name"
  console = Console.fake_input(["bo 13:00-17:00", "ada 09:00-12:30", "ada 13:15-17:00", "oops"])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["ada: 7:15", "bo: 4:00"]
  assert console.printed_errors == ["line 4: can't read oops"]
end

test "overlapping shifts are rejected against the earliest counted line"
  console = Console.fake_input([
    "ada 13:00-14:00",
    "",
    "ada 09:00-10:00",
    "ada 09:30-13:30",
    "ada 10:00-13:00",
    "ada 09:15-09:45",
    "ada 08:00-15:00",
    "ada 09:00-10:00"
  ])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["ada: 5:00"]
  assert console.printed_errors == [
    "line 4: overlaps line 1",
    "line 6: overlaps line 3",
    "line 7: overlaps line 1",
    "line 8: overlaps line 3"
  ]
end

test "over filters strictly and formats total and extra minutes in name order"
  console = Console.fake_input([
    "zoe 00:00-10:05",
    "bo 09:00-17:00",
    "ada 08:00-12:30",
    "cy 09:00-16:59",
    "ada 12:30-17:30",
    "dan 08:00-16:01"
  ])
  assert main(Args.of(["--over", "8"]), console) == 0
  assert console.printed == [
    "ada: 9:30 (1:30 over)",
    "dan: 8:01 (0:01 over)",
    "zoe: 10:05 (2:05 over)"
  ]
  assert console.printed_errors == []
end

test "usage errors leave standard input unread"
  for arguments in [
    ["--over"],
    ["8"],
    ["--other", "8"],
    ["--over", "8", "extra"],
    ["--over", "-1"],
    ["--over", "1.5"],
    ["--over", ""],
    ["--over", "+8"],
    ["--over", " 8"],
    ["--over", "eight"]
  ]
    console = Console.fake_input(["ada 09:00-12:00"])
    assert main(Args.of(arguments), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: timesheet [--over HOURS]"]
    assert console.read_line == Some("ada 09:00-12:00")
  end
end

test "zero threshold keeps exact names separate and still reports input errors"
  console = Console.fake_input([
    "ada 09:00-10:00",
    "Ada 09:00-10:00",
    "ada 09:59-10:30",
    "ada 10:00-10:01",
    "bad"
  ])
  assert main(Args.of(["--over", "000"]), console) == 1
  assert console.printed == ["Ada: 1:00 (1:00 over)", "ada: 1:01 (1:01 over)"]
  assert console.printed_errors == ["line 3: overlaps line 1", "line 5: can't read bad"]
end

test "large whole number thresholds do not overflow"
  for threshold in ["9223372036854775807", "999999999999999999999999999999999999"]
    console = Console.fake_input(["ada 00:00-23:59"])
    assert main(Args.of(["--over", threshold]), console) == 0
    assert console.printed == []
    assert console.printed_errors == []
  end
end

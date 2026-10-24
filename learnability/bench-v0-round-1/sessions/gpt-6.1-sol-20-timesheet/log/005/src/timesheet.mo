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
fn hours(minutes: Int) : Text
  examples
    hours(210) == "3:30"
    hours(5) == "0:05"
    hours(1500) == "25:00"
  end

  "#{minutes / 60}:#{"#{minutes % 60}".pad_left(2).replace(" ", "0")}"
end

fn overlapping_line(previous: List((Int, Shift)), shift: Shift) : Option(Int)
  for (number, earlier) in previous
    return Some(number) if shift.start < earlier.finish and earlier.start < shift.finish
  end
  None
end

fn over_minutes(arguments: List(Text)) : Result(Option(Int), Unit)
  case arguments
    []: Ok(None)
    ["--over", text]:
      return Error(Unit) if text.empty?
      var value = 0
      for digit in text.chars
        return Error(Unit) if not digit.ascii_digit?
        value = value.saturating_mul(10).saturating_add(digit.code - '0'.code)
      end
      # A threshold beyond Int cannot be exceeded by shifts within one day.
      Ok(Some(value.saturating_mul(60)))
    _: Error(Unit)
  end
end

fn main(args: Args, console: Console) : Int
  over = case over_minutes(args.list)
    Ok(limit): limit
    Error(_):
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
        case overlapping_line(previous, shift)
          Some(earlier):
            console.error("line #{number}: overlaps line #{earlier}")
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
    total = totals.get(name) or 0
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

test "only counted shifts overlap, with the earliest input line reported"
  console = Console.fake_input([
    "ada 13:00-14:00",
    "ada 09:00-10:00",
    "ada 09:30-13:30",
    "ada 10:00-11:00",
    "ada 12:00-13:00",
    "Ada 09:00-14:00",
    "ada 13:00-14:00",
    "bad",
    " ",
    "ada 08:59-09:01"
  ])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["Ada: 5:00", "ada: 4:00"]
  assert console.printed_errors == [
    "line 3: overlaps line 1",
    "line 7: overlaps line 1",
    "line 8: can't read bad",
    "line 10: overlaps line 2"
  ]
end

test "over prints only totals strictly above the threshold, with their extra minutes"
  console = Console.fake_input([
    "zed 08:00-18:05",
    "bo 09:00-17:00",
    "ada 08:00-12:30",
    "ada 13:00-18:00",
    "cy 09:00-16:59",
    "bex 00:00-08:01"
  ])
  assert main(Args.of(["--over", "8"]), console) == 0
  assert console.printed == [
    "ada: 9:30 (1:30 over)",
    "bex: 8:01 (0:01 over)",
    "zed: 10:05 (2:05 over)"
  ]
  assert console.printed_errors == []
end

test "zero and leading zero thresholds include even a single minute"
  for text in ["0", "0000"]
    console = Console.fake_input(["ada 09:00-09:01"])
    assert main(Args.of(["--over", text]), console) == 0
    assert console.printed == ["ada: 0:01 (0:01 over)"]
    assert console.printed_errors == []
  end
end

test "large whole thresholds are valid and do not overflow"
  for text in [
    "24",
    "153722867280912931",
    "9223372036854775807",
    "999999999999999999999999999999999"
  ]
    console = Console.fake_input(["ada 00:00-23:59"])
    assert main(Args.of(["--over", text]), console) == 0
    assert console.printed == []
    assert console.printed_errors == []
  end
end

test "over still rejects overlaps and reports unreadable lines for filtered people"
  console = Console.fake_input(["ada 09:00-17:00", "ada 16:00-18:00", "oops"])
  assert main(Args.of(["--over", "8"]), console) == 1
  assert console.printed == []
  assert console.printed_errors == ["line 2: overlaps line 1", "line 3: can't read oops"]
end

test "invalid arguments report usage without consuming any input"
  for arguments in [
    ["extra"],
    ["--over"],
    ["--over=8"],
    ["--bad", "8"],
    ["--over", "8", "extra"],
    ["--over", ""],
    ["--over", "-1"],
    ["--over", "+1"],
    ["--over", "1.0"],
    ["--over", "1e2"],
    ["--over", " 1"],
    ["--over", "1 "],
    ["--over", "١"],
    ["--over", "999999999999999999999999999x"]
  ]
    console = Console.fake_input(["ada 09:00-10:00"])
    assert main(Args.of(arguments), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: timesheet [--over HOURS]"]
    assert console.read_line == Some("ada 09:00-10:00")
  end
end

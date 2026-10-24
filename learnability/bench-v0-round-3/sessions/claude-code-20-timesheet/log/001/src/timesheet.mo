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

## Reads the command line: `None` for a usage error, otherwise the limit in minutes, if any.
## A limit too large for an `Int` becomes the largest `Int`, which no total exceeds.
fn parse_args(args: List(Text)) : Option(Option(Int))
  examples
    parse_args([]) == Some(None)
    parse_args(["--over", "8"]) == Some(Some(480))
    parse_args(["--over", "0"]) == Some(Some(0))
    parse_args(["--over", "99999999999999999999"]) == Some(Some(9_223_372_036_854_775_807))
    parse_args(["--over"]) == None
    parse_args(["--over", "-1"]) == None
    parse_args(["--over", "1.5"]) == None
    parse_args(["--over", ""]) == None
    parse_args(["--over", "8", "x"]) == None
    parse_args(["8"]) == None
  end

  case args
    []: Some(None)
    ["--over", text] if not text.empty? and text.chars.all?(fn(c) c.ascii_digit? end):
      limit = case text.to_int
        Some(n): n.checked_mul(60) or 9_223_372_036_854_775_807
        None: 9_223_372_036_854_775_807
      end
      Some(Some(limit))
    _: None
  end
end

## Whether two shifts share a minute; shifts that only touch don't.
fn overlaps?(a: Shift, b: Shift) : Bool
  examples
    overlaps?(
      Shift(name: "ada", start: 540, finish: 720),
      Shift(name: "ada", start: 700, finish: 780)
    )
    not overlaps?(
      Shift(name: "ada", start: 540, finish: 720),
      Shift(name: "ada", start: 720, finish: 780)
    )
    not overlaps?(
      Shift(name: "ada", start: 720, finish: 780),
      Shift(name: "ada", start: 540, finish: 720)
    )
    overlaps?(
      Shift(name: "ada", start: 540, finish: 780),
      Shift(name: "ada", start: 600, finish: 660)
    )
  end

  a.start < b.finish and b.start < a.finish
end

fn main(args: Args, console: Console) : Int
  case parse_args(args.list)
    None:
      console.error("usage: timesheet [--over HOURS]")
      2
    Some(limit): report(limit, console)
  end
end

## Reads the shifts, prints the totals over `limit` minutes (or all of them), and gives the exit status.
fn report(limit: Option(Int), console: Console) : Int
  var counted = {}
  var status = 0
  var number = 0
  while console.read_line is Some(line)
    number += 1
    continue if line.trim.empty?
    case parse_shift(line)
      Some(shift):
        earlier = counted.get(shift.name) or []
        case earlier.find(fn(entry) overlaps?(entry.1, shift) end)
          Some((other, _)):
            console.error("line #{number}: overlaps line #{other}")
            status = 1
          None: counted = counted.set(shift.name, earlier.append((number, shift)))
        end
      None:
        console.error("line #{number}: can't read #{line}")
        status = 1
    end
  end
  for name in counted.keys.sort
    shifts = counted.get(name) or []
    total = shifts.reduce(0, fn(sum, entry) sum + entry.1.finish - entry.1.start end)
    case limit
      None: console.print("#{name}: #{hours(total)}")
      Some(minutes) if total > minutes: console.print(
        "#{name}: #{hours(total)} (#{hours(total - minutes)} over)"
      )
      Some(_): continue
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

test "a shift sharing a minute with a counted shift of the same person is not counted"
  console = Console.fake_input([
    "ada 09:00-12:00",
    "ada 12:00-13:00",
    "ada 11:00-12:30",
    "Ada 10:00-11:00",
    "bo 10:00-11:00",
    "ada 12:45-14:00",
    "ada 11:30-12:15"
  ])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["Ada: 1:00", "ada: 4:00", "bo: 1:00"]
  assert console.printed_errors == [
    "line 3: overlaps line 1",
    "line 6: overlaps line 2",
    "line 7: overlaps line 1"
  ]
end

test "a shift overlapping only an uncounted shift is counted"
  console = Console.fake_input(["ada 09:00-10:00", "ada 09:30-11:00", "ada 10:00-11:00"])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["ada: 2:00"]
  assert console.printed_errors == ["line 2: overlaps line 1"]
end

test "with --over, only people over the limit are printed, with how much over"
  console = Console.fake_input(["ada 08:00-17:30", "bo 09:00-17:00", "cy 09:00-17:01"])
  assert main(Args.of(["--over", "8"]), console) == 0
  assert console.printed == ["ada: 9:30 (1:30 over)", "cy: 8:01 (0:01 over)"]
  assert console.printed_errors == []
end

test "with --over 0, everyone who worked is printed"
  console = Console.fake_input(["bo 09:00-09:05"])
  assert main(Args.of(["--over", "0"]), console) == 0
  assert console.printed == ["bo: 0:05 (0:05 over)"]
end

test "other arguments are a usage error, and nothing is read"
  for args in [
    ["--over"],
    ["--over", "-1"],
    ["--over", "x"],
    ["8"],
    ["--over", "8", "--over", "9"],
    ["--under", "8"]
  ]
    console = Console.fake_input(["ada 09:00-10:00", "oops"])
    assert main(Args.of(args), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: timesheet [--over HOURS]"]
    assert console.read_line == Some("ada 09:00-10:00")
  end
end

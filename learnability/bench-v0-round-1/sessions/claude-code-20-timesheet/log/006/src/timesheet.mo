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

## A shift that was counted, with the number of the line it came from.
struct Counted
  line: Int
  shift: Shift
end

## Which people to print: everyone, or only those who worked more than some whole hours.
enum Report
  Everyone
  Over(Int)
end

## Reads the command-line arguments, or gives `None` when they are a usage error.
fn parse_args(args: List(Text)) : Option(Report)
  examples
    parse_args([]) == Some(Everyone)
    parse_args(["--over", "8"]) == Some(Over(8))
    parse_args(["--over", "0"]) == Some(Over(0))
    parse_args(["--over", "-1"]) == None
    parse_args(["--over", "8.5"]) == None
    parse_args(["--over"]) == None
    parse_args(["--over", "8", "x"]) == None
    parse_args(["8"]) == None
  end

  case args
    []: Some(Everyone)
    ["--over", text]:
      n = try text.to_int
      if n >= 0 and not text.starts_with?("-"): Some(Over(n)) else: None
    _: None
  end
end

## Whether two shifts share a minute; shifts that only touch don't.
fn overlaps?(a: Shift, b: Shift) : Bool
  promise "two shifts overlap exactly when some minute lies inside both, whichever comes first"
    result == overlaps?(b, a)
    result == (a.start.max(b.start) < a.finish.min(b.finish))
    allows a: Shift(name: "ada", start: 540, finish: 720), b: Shift(
      name: "ada",
      start: 600,
      finish: 780
    ), result: true
    forbids a: Shift(name: "ada", start: 540, finish: 720), b: Shift(
      name: "ada",
      start: 720,
      finish: 780
    ), result: true
  end

  examples
    overlaps?(
      Shift(name: "ada", start: 540, finish: 720),
      Shift(name: "ada", start: 719, finish: 780)
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
      Shift(name: "ada", start: 540, finish: 720),
      Shift(name: "ada", start: 600, finish: 660)
    )
  end

  a.start < b.finish and b.start < a.finish
end

## The line printed for a person who worked `total` minutes, or `None` when the report leaves them out.
fn report_line(name: Text, total: Int, report: Report) : Option(Text)
  examples
    report_line("ada", 435, Everyone) == Some("ada: 7:15")
    report_line("ada", 570, Over(8)) == Some("ada: 9:30 (1:30 over)")
    report_line("ada", 480, Over(8)) == None
    report_line("ada", 481, Over(8)) == Some("ada: 8:01 (0:01 over)")
    report_line("ada", 1, Over(0)) == Some("ada: 0:01 (0:01 over)")
    report_line("ada", 600, Over(9_223_372_036_854_775_807)) == None
  end

  case report
    Everyone: Some("#{name}: #{hours(total)}")
    Over(limit_hours):
      limit = limit_hours.checked_mul(60) or total
      if total > limit: Some("#{name}: #{hours(total)} (#{hours(total - limit)} over)") else: None
  end
end

fn main(args: Args, console: Console) : Int
  report = case parse_args(args.list)
    Some(r): r
    None:
      console.error("usage: timesheet [--over HOURS]")
      return 2
  end
  # Each person's counted shifts, with the line each came from, in line order.
  var counted = {}
  var status = 0
  var number = 0
  while console.read_line is Some(line)
    number += 1
    continue if line.trim.empty?
    case parse_shift(line)
      Some(shift):
        earlier = counted.get(shift.name) or []
        case earlier.find(fn(entry) overlaps?(entry.shift, shift) end)
          Some(other):
            console.error("line #{number}: overlaps line #{other.line}")
            status = 1
          None: counted = counted.set(
            shift.name,
            earlier.append(Counted(line: number, shift: shift))
          )
        end
      None:
        console.error("line #{number}: can't read #{line}")
        status = 1
    end
  end
  for name in counted.keys.sort
    shifts = counted.get(name) or []
    total = shifts.reduce(0, fn(sum, entry) sum + entry.shift.finish - entry.shift.start end)
    if report_line(name, total, report) is Some(text)
      console.print(text)
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

test "a shift that overlaps a counted shift of the same person is reported and not counted"
  console = Console.fake_input([
    "ada 09:00-12:00",
    "ada 12:00-13:00",
    "bo 09:30-10:00",
    "Ada 10:00-11:00",
    "ada 11:30-12:30",
    "ada 12:15-12:45",
    "ada 08:00-09:01"
  ])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["Ada: 1:00", "ada: 4:00", "bo: 0:30"]
  assert console.printed_errors == [
    "line 5: overlaps line 1",
    "line 6: overlaps line 2",
    "line 7: overlaps line 1"
  ]
end

test "a shift that overlaps only an uncounted shift is counted"
  console = Console.fake_input(["ada 09:00-10:00", "ada 09:30-11:00", "ada 10:30-11:30"])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["ada: 2:00"]
  assert console.printed_errors == ["line 2: overlaps line 1"]
end

test "shifts that only touch are all counted"
  console = Console.fake_input(["ada 12:00-13:00", "ada 09:00-12:00", "ada 13:00-14:00"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["ada: 5:00"]
  assert console.printed_errors == []
end

test "--over prints only the people over the limit, with how much over"
  console = Console.fake_input([
    "ada 08:00-12:00",
    "ada 13:00-18:30",
    "bo 09:00-17:00",
    "cy 09:00-17:01"
  ])
  assert main(Args.of(["--over", "8"]), console) == 0
  assert console.printed == ["ada: 9:30 (1:30 over)", "cy: 8:01 (0:01 over)"]
  assert console.printed_errors == []
end

test "--over still reports unreadable and overlapping lines"
  console = Console.fake_input(["ada 09:00-10:00", "ada 09:30-10:30", "nope"])
  assert main(Args.of(["--over", "0"]), console) == 1
  assert console.printed == ["ada: 1:00 (1:00 over)"]
  assert console.printed_errors == ["line 2: overlaps line 1", "line 3: can't read nope"]
end

test "other arguments are a usage error and no input is read"
  for list in [["--over"], ["--over", "-1"], ["--over", "x"], ["8"], ["--over", "8", "--over", "9"]]
    console = Console.fake_input(["ada 09:00-10:00"])
    assert main(Args.of(list), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: timesheet [--over HOURS]"]
    assert console.read_line == Some("ada 09:00-10:00")
  end
end

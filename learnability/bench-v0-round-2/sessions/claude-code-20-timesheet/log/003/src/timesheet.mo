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

## Reads the arguments: `None` for a usage error, else the `--over` limit in hours, if any.
fn parse_args(args: List(Text)) : Option(Option(Int))
  examples
    parse_args([]) == Some(None)
    parse_args(["--over", "8"]) == Some(Some(8))
    parse_args(["--over", "0"]) == Some(Some(0))
    parse_args(["--over", "-1"]) == None
    parse_args(["--over"]) == None
    parse_args(["--over", "8", "x"]) == None
    parse_args(["8"]) == None
  end

  case args
    []: Some(None)
    ["--over", text]:
      case text.to_int
        Some(limit) if limit >= 0: Some(Some(limit))
        Some(_) | None: None
      end
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
    overlaps?(
      Shift(name: "ada", start: 600, finish: 660),
      Shift(name: "ada", start: 540, finish: 720)
    )
  end

  a.start < b.finish and b.start < a.finish
end

fn main(args: Args, console: Console) : Int
  over = case parse_args(args.list)
    Some(limit): limit
    None:
      console.error("usage: timesheet [--over HOURS]")
      return 2
  end
  var counted = []
  var totals = {}
  var status = 0
  var number = 0
  while console.read_line is Some(line)
    number += 1
    continue if line.trim.empty?
    case parse_shift(line)
      Some(shift):
        clash = counted.find(fn(entry)
          entry.1.name == shift.name and overlaps?(entry.1, shift)
        end)
        case clash
          Some((earlier, _)):
            console.error("line #{number}: overlaps line #{earlier}")
            status = 1
          None:
            counted = counted.append((number, shift))
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
        minutes = limit.saturating_mul(60)
        if total > minutes
          console.print("#{name}: #{hours(total)} (#{hours(total - minutes)} over)")
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

test "a shift overlapping a counted shift of the same person is not counted"
  console = Console.fake_input([
    "ada 09:00-12:00",
    "ada 12:00-13:00",
    "bo 09:30-10:00",
    "ada 11:00-12:30",
    "ada 10:00-11:00",
    "Ada 09:00-10:00"
  ])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["Ada: 1:00", "ada: 4:00", "bo: 0:30"]
  assert console.printed_errors == ["line 4: overlaps line 1", "line 5: overlaps line 1"]
end

test "an uncounted shift is not what a later shift overlaps"
  console = Console.fake_input([
    "ada 09:00-10:00",
    "ada 09:30-12:00",
    "ada 11:00-11:30",
    "ada 08:00-13:00"
  ])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["ada: 1:30"]
  assert console.printed_errors == ["line 2: overlaps line 1", "line 4: overlaps line 1"]
end

test "without problems the exit status is 0"
  console = Console.fake_input(["ada 09:00-12:00", "", "ada 12:00-13:00"])
  assert main(Args.of([]), console) == 0
  assert console.printed == ["ada: 4:00"]
  assert console.printed_errors == []
end

test "--over prints only those who worked more, with how much more"
  console = Console.fake_input([
    "ada 08:00-17:30",
    "bo 09:00-17:00",
    "cy 09:00-17:01",
    "dee 09:00-10:00"
  ])
  assert main(Args.of(["--over", "8"]), console) == 0
  assert console.printed == ["ada: 9:30 (1:30 over)", "cy: 8:01 (0:01 over)"]
end

test "--over 0 prints everyone who worked"
  console = Console.fake_input(["bo 09:00-09:05", "ada 10:00-12:00"])
  assert main(Args.of(["--over", "0"]), console) == 0
  assert console.printed == ["ada: 2:00 (2:00 over)", "bo: 0:05 (0:05 over)"]
end

test "--over with a huge limit prints nobody"
  console = Console.fake_input(["ada 00:00-23:59"])
  assert main(Args.of(["--over", "9223372036854775807"]), console) == 0
  assert console.printed == []
end

test "other arguments are a usage error, and nothing is read"
  for args in [
    ["--over"],
    ["--over", "x"],
    ["--over", "-2"],
    ["--over", "1.5"],
    ["--over", "8", "9"],
    ["-v"],
    ["--OVER", "8"]
  ]
    console = Console.fake_input(["ada 09:00-12:00", "oops"])
    assert main(Args.of(args), console) == 2
    assert console.printed == []
    assert console.printed_errors == ["usage: timesheet [--over HOURS]"]
    assert console.read_line == Some("ada 09:00-12:00")
  end
end

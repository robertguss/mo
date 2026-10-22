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

## A shift that was counted, and the line it was on.
struct Counted
  shift: Shift
  line: Int
end

## The line of the first counted shift of the same person that shares a minute with `shift`.
fn overlap(counted: List(Counted), shift: Shift) : Option(Int)
  examples
    overlap(
      [Counted(shift: Shift(name: "ada", start: 540, finish: 600), line: 1)],
      Shift(name: "ada", start: 590, finish: 700)
    ) ==
      Some(1)
    overlap(
      [Counted(shift: Shift(name: "ada", start: 540, finish: 600), line: 1)],
      Shift(name: "ada", start: 600, finish: 700)
    ) ==
      None
    overlap(
      [Counted(shift: Shift(name: "bo", start: 540, finish: 600), line: 1)],
      Shift(name: "ada", start: 540, finish: 600)
    ) ==
      None
  end

  found = counted.find(fn(c)
    c.shift.name == shift.name and shift.start <= c.shift.finish and c.shift.start <= shift.finish
  end)
  found.map(fn(c) c.line end)
end

## The number of hours after `--over`, or `None` when the arguments are anything else.
fn parse_args(args: List(Text)) : Option(Option(Int))
  examples
    parse_args([]) == Some(None)
    parse_args(["--over", "8"]) == Some(Some(8))
    parse_args(["--over", "-1"]) == None
    parse_args(["--over"]) == None
  end

  case args
    []: Some(None)
    ["--over", text]:
      n = try text.to_int
      if n >= 0: Some(Some(n)) else: None
    _: None
  end
end

fn main(args: Args, console: Console) : Int
  over = case parse_args(args.list)
    Some(o): o
    None:
      console.error("usage: timesheet [--over HOURS]")
      return 2
  end
  var counted = []
  var status = 0
  var number = 0
  while console.read_line is Some(line)
    number += 1
    continue if line.trim.empty?
    case parse_shift(line)
      Some(shift):
        case overlap(counted, shift)
          Some(earlier):
            console.error("line #{number}: overlaps line #{earlier}")
            status = 1
          None: counted = counted.append(Counted(shift: shift, line: number))
        end
      None:
        console.error("line #{number}: can't read #{line}")
        status = 1
    end
  end
  var totals = {}
  for c in counted
    totals = totals.update(c.shift.name, 0, fn(total) total + c.shift.finish - c.shift.start end)
  end
  for name in totals.keys.sort
    total = totals.get(name) or 0
    case over
      None: console.print("#{name}: #{hours(total)}")
      Some(limit) if total > limit * 60: console.print(
        "#{name}: #{hours(total)} (#{hours(total - limit * 60)} over)"
      )
      Some(_):
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

test "a shift that overlaps one already counted is skipped"
  console = Console.fake_input([
    "ada 09:00-12:00",
    "ada 11:00-13:00",
    "ada 12:00-13:00",
    "bo 09:00-10:00"
  ])
  assert main(Args.of([]), console) == 1
  assert console.printed == ["ada: 4:00", "bo: 1:00"]
  assert console.printed_errors == ["line 2: overlaps line 1"]
end

test "with --over, only people over the hours are printed"
  console = Console.fake_input(["ada 08:00-17:30", "bo 09:00-17:00", "cy 09:00-17:01"])
  assert main(Args.of(["--over", "8"]), console) == 0
  assert console.printed == ["ada: 9:30 (1:30 over)", "cy: 8:01 (0:01 over)"]
end

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

fn main(console: Console) : Int
  var totals = {}
  var status = 0
  var number = 0
  while console.read_line is Some(line)
    number += 1
    continue if line.trim.empty?
    case parse_shift(line)
      Some(shift): totals = totals.update(
        shift.name,
        0,
        fn(total) total + shift.finish - shift.start end
      )
      None:
        console.error("line #{number}: can't read #{line}")
        status = 1
    end
  end
  for name in totals.keys.sort
    console.print("#{name}: #{hours(totals.get(name) or 0)}")
  end
  status
end

test "each person's hours are totalled, in order of name"
  console = Console.fake_input(["bo 13:00-17:00", "ada 09:00-12:30", "ada 13:15-17:00", "oops"])
  assert main(console) == 1
  assert console.printed == ["ada: 7:15", "bo: 4:00"]
  assert console.printed_errors == ["line 4: can't read oops"]
end

test "overlapping shifts are rejected against the earliest counted line"
  console = Console.fake_input([
    "ada 13:00-14:00",
    "",
    "ada 09:00-10:00",
    "ada 09:30-13:30",
    "ada 10:00-13:00"
  ])
  assert main(console) == 1
  assert console.printed == ["ada: 5:00"]
  assert console.printed_errors == ["line 4: overlaps line 1"]
end

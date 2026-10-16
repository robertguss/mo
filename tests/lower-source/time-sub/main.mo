module Main

fn must_parse(text: Text) : Time
  case Time.parse(text)
    Some(value): value
    None: stop "invalid time"
  end
end

fn main(console: Console) : Int
  t0 = must_parse("2026-01-01T00:00:00Z")
  t1 = t0 + 10.seconds
  t2 = t1 - 250.milliseconds
  zero_diff = t0 - t0
  pos_diff = t1 - t0
  neg_diff = t0 - t2
  combo = (3.seconds + 500.milliseconds - 1.seconds) * 2
  neg_dur = -combo
  earliest = must_parse("0001-01-01T00:00:00Z")
  latest = must_parse("9999-12-31T23:59:59.999999Z")
  span = latest - earliest
  reverse_span = earliest - latest
  console.print("#{pos_diff.in_milliseconds}/#{neg_diff.in_milliseconds}/#{combo.in_seconds}/#{neg_dur.in_milliseconds}")
  if zero_diff == 0.seconds and
    pos_diff == 10.seconds and
    pos_diff.in_milliseconds == 10000 and
    neg_diff.in_milliseconds == -9750 and
    combo == 5.seconds and
    neg_dur == -5.seconds and
    earliest + span == latest and
    latest + reverse_span == earliest and
    reverse_span == -span and
    1.minutes == 60.seconds and
    1.hours == 60.minutes and
    1.days == 24.hours and
    latest.checked_add(1.milliseconds) is None: 0 else: 1
end

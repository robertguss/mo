## Time and Duration of reference sections 3.9 and 14, and Clock's fixed fake. Every operation is
## an instruction or a C primitive: Mo code can neither build a Time from its parts nor read them,
## and the VM writes the same text forms inside other values. These tests pin what the reference
## says each one does.
module Time

test "durations count whole microseconds made from an Int"
  assert 5.seconds == 5000.milliseconds
  assert 3.minutes == 180.seconds
  assert 2.hours == 120.minutes
  assert 7.days == 168.hours
  assert 0.days == 0.milliseconds
  assert (-3).seconds == -(3.seconds)
  assert 9223372036854.seconds.in_milliseconds == 9223372036854000
end

test "durations add, subtract, negate and multiply by an Int"
  assert 1.second + 500.milliseconds == 1500.milliseconds
  assert 1.second - 1500.milliseconds == (-500).milliseconds
  assert -(2.seconds) == (-2).seconds
  assert -((-2).seconds) == 2.seconds
  assert 200.milliseconds * 3 == 600.milliseconds
  assert 200.milliseconds * (-3) == (-600).milliseconds
  assert 1.day * 0 == 0.seconds
end

test "in_milliseconds rounds toward zero and in_seconds is exact"
  start = Time.parse("2026-01-01T00:00:00Z") or stop "valid time"
  later = Time.parse("2026-01-01T00:00:00.001999Z") or stop "valid time"
  assert (later - start).in_milliseconds == 1
  assert (start - later).in_milliseconds == -1
  assert (later - start).in_seconds == 0.001999
  assert (start - later).in_seconds == -0.001999
  assert 1500.milliseconds.in_seconds == 1.5
  assert (-1500).milliseconds.in_milliseconds == -1500
  assert 2.hours.in_seconds == 7200
  assert 0.seconds.in_seconds == 0
end

test "a duration's text is its seconds, as short as it can be"
  start = Time.parse("2026-01-01T00:00:00Z") or stop "valid time"
  later = Time.parse("2026-01-01T00:00:00.000001Z") or stop "valid time"
  assert 0.seconds.to_text == "0s"
  assert 1500.milliseconds.to_text == "1.5s"
  assert (-1500).milliseconds.to_text == "-1.5s"
  assert 90.minutes.to_text == "5400s"
  assert 120.milliseconds.to_text == "0.12s"
  assert (later - start).to_text == "0.000001s"
  assert (start - later).to_text == "-0.000001s"
  assert "#{2.seconds}" == "2s"
end

test "durations compare and order"
  assert 1.second > 999.milliseconds
  assert (-1).seconds < 0.seconds
  assert 60.seconds <= 1.minute
  assert 60.seconds >= 1.minute
  assert 60.seconds != 61.seconds
  assert 2.seconds.compare(1.second) == Greater
  assert [3.seconds, (-1).seconds, 1.millisecond].sort == [(-1).seconds, 1.millisecond, 3.seconds]
end

test "adding or subtracting a duration gives a time, and two times give a duration"
  t = Time.parse("2026-10-12T02:15:00Z") or stop "valid time"
  assert (t + 30.minutes).to_text == "2026-10-12T02:45:00Z"
  assert (t - 1.hour).to_text == "2026-10-12T01:15:00Z"
  assert t + (-1).hours == t - 1.hour
  assert (t + 90.minutes) - t == 90.minutes
  assert t - (t + 90.minutes) == (-90).minutes
  assert t - t == 0.seconds
  assert (t + 1500.milliseconds).to_text == "2026-10-12T02:15:01.5Z"
end

test "time arithmetic follows the calendar across months, years and leap days"
  new_year = Time.parse("2026-12-31T23:30:00Z") or stop "valid time"
  assert (new_year + 1.hour).to_text == "2027-01-01T00:30:00Z"
  leap = Time.parse("2024-02-28T12:00:00Z") or stop "valid time"
  assert (leap + 1.day).to_text == "2024-02-29T12:00:00Z"
  assert (leap + 2.days).to_text == "2024-03-01T12:00:00Z"
  century = Time.parse("2100-02-28T12:00:00Z") or stop "valid time"
  assert (century + 1.day).to_text == "2100-03-01T12:00:00Z"
  fourth = Time.parse("2000-02-28T12:00:00Z") or stop "valid time"
  assert (fourth + 1.day).to_text == "2000-02-29T12:00:00Z"
  plain = Time.parse("2026-02-28T12:00:00Z") or stop "valid time"
  assert (plain + 1.day).to_text == "2026-03-01T12:00:00Z"
  assert (plain + 365.days).to_text == "2027-02-28T12:00:00Z"
end

test "times compare and order"
  early = Time.parse("2026-10-12T02:15:00Z") or stop "valid time"
  late = Time.parse("2026-10-12T02:15:00.000001Z") or stop "valid time"
  assert early < late
  assert late > early
  assert early <= early
  assert early != late
  assert early.compare(late) == Less
  assert [late, early].sort == [early, late]
  assert [late, early].min == Some(early)
end

test "subtracting the first time from the last never overflows"
  first = Time.parse("0001-01-01T00:00:00Z") or stop "first instant"
  last = Time.parse("9999-12-31T23:59:59.999999Z") or stop "last instant"
  assert (last - first).in_seconds == 315537897599.999999
  assert (first - last).to_text == "-315537897599.999999s"
  assert first + (last - first) == last
  assert last + (first - last) == first
end

test "checked_add gives None for a time outside the years 1 to 9999"
  first = Time.parse("0001-01-01T00:00:00Z") or stop "first instant"
  last = Time.parse("9999-12-31T23:59:59.999999Z") or stop "last instant"
  assert first.checked_add((-1).seconds) == None
  assert first.checked_add(0.seconds) == Some(first)
  assert first.checked_add(1.second) == Time.parse("0001-01-01T00:00:01Z")
  assert last.checked_add(1.millisecond) == None
  assert last.checked_add(-(1.day)) == Time.parse("9999-12-30T23:59:59.999999Z")
  assert first.checked_add(last - first) == Some(last)
  assert last.checked_add(first - last) == Some(first)
  near = Time.parse("9999-12-31T23:59:59.998Z") or stop "valid time"
  assert near.checked_add(1.millisecond) == Time.parse("9999-12-31T23:59:59.999Z")
  assert near.checked_add(2.milliseconds) == None
end

test "to_text writes RFC 3339 in UTC with the shortest fraction of a second"
  assert (Time.parse("2026-10-12T02:15:00Z") or stop "t").to_text == "2026-10-12T02:15:00Z"
  assert (Time.parse("2026-10-12T02:15:00.5Z") or stop "t").to_text == "2026-10-12T02:15:00.5Z"
  assert (Time.parse("2026-10-12T02:15:00.120Z") or stop "t").to_text == "2026-10-12T02:15:00.12Z"
  assert (Time.parse("2026-10-12T02:15:00.000100Z") or stop "t").to_text ==
    "2026-10-12T02:15:00.0001Z"
  assert (Time.parse("2026-10-12T02:15:00.000000Z") or stop "t").to_text == "2026-10-12T02:15:00Z"
  assert (Time.parse("0001-01-01T00:00:00Z") or stop "t").to_text == "0001-01-01T00:00:00Z"
  assert (Time.parse("9999-12-31T23:59:59.999999Z") or stop "t").to_text ==
    "9999-12-31T23:59:59.999999Z"
  assert "#{Time.parse("0042-03-04T05:06:07Z")}" == "Some(0042-03-04T05:06:07Z)"
end

test "parse reads T or t, Z, z or an offset, and converts to UTC"
  utc = Time.parse("2026-10-12T02:15:00.5Z")
  assert utc != None
  assert Time.parse("2026-10-12T04:15:00.5+02:00") == utc
  assert Time.parse("2026-10-12t02:15:00.5z") == utc
  assert Time.parse("2026-10-12T02:15:00.5-00:00") == utc
  assert Time.parse("2026-10-12T02:15:00.5+00:00") == utc
  assert Time.parse("2026-10-11T23:45:00.5-02:30") == utc
  assert Time.parse("2026-01-01T01:00:00+02:00") == Time.parse("2025-12-31T23:00:00Z")
  assert Time.parse("2026-10-12T02:15:00+23:59") == Time.parse("2026-10-11T02:16:00Z")
  assert Time.parse("2024-02-29T00:00:00Z") != None
  assert Time.parse("2000-02-29T00:00:00Z") != None
end

test "parse gives None for a time outside the years 1 to 9999, even through an offset"
  assert Time.parse("0001-01-01T00:30:00-01:00") == Time.parse("0001-01-01T01:30:00Z")
  assert Time.parse("0001-01-01T00:30:00+01:00") == None
  assert Time.parse("9999-12-31T22:30:00-01:00") == Time.parse("9999-12-31T23:30:00Z")
  assert Time.parse("9999-12-31T23:30:00-01:00") == None
  assert Time.parse("0000-12-31T23:59:59Z") == None
  assert Time.parse("10000-01-01T00:00:00Z") == None
end

test "parse gives None for a leap second, a 7th fraction digit, or anything else"
  assert Time.parse("2016-12-31T23:59:60Z") == None
  assert Time.parse("2026-10-12T02:15:00.1234567Z") == None
  assert Time.parse("2026-10-12T02:15:00.123456Z") != None
  assert Time.parse("2026-10-12T02:15:00.Z") == None
  assert Time.parse("2026-10-12T02:15:00") == None
  assert Time.parse("2026-10-12 02:15:00Z") == None
  assert Time.parse("2026-10-12T02:15Z") == None
  assert Time.parse(" 2026-10-12T02:15:00Z") == None
  assert Time.parse("2026-10-12T02:15:00Z ") == None
  assert Time.parse("") == None
  assert Time.parse("2026-13-01T00:00:00Z") == None
  assert Time.parse("2026-04-31T00:00:00Z") == None
  assert Time.parse("2026-02-29T00:00:00Z") == None
  assert Time.parse("1900-02-29T00:00:00Z") == None
  assert Time.parse("2026-10-12T24:00:00Z") == None
  assert Time.parse("2026-10-12T02:60:00Z") == None
  assert Time.parse("2026-10-12T02:15:00+24:00") == None
  assert Time.parse("2026-10-12T02:15:00+02") == None
end

test "a fixed clock tells the time it was given"
  t = Time.parse("2026-10-12T02:15:00Z") or stop "valid time"
  clock = Clock.fixed(t)
  assert clock.now == t
  assert clock.now - t == 0.seconds
  assert Clock.fixed(t + 1.day).now == t + 1.day
end

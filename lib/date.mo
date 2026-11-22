## Validated Gregorian calendar dates (years 1..9999) with exact integer calendar arithmetic,
## ISO-8601 `"YYYY-MM-DD"` formatting and parsing, and custom `Text` / `Json` abilities.
module Date
expose Date,
Weekday(..),
from_ymd,
parse_iso,
from_time_utc,
to_iso,
to_text,
to_json,
from_json,
year,
month,
day,
year_month,
weekday,
days_in_month,
add_days,
add_months,
days_between,
first_of_month,
end_of_month,
short_label
use Json

## Opaque validated Gregorian date in years 1..9999. Field order matches chronological order.
struct Date
  year: Int
  month: Int
  day: Int
end

enum Weekday
  Monday
  Tuesday
  Wednesday
  Thursday
  Friday
  Saturday
  Sunday
end

min_epoch_days = -719162
max_epoch_days = 2932896

fn leap_year?(y: Int) : Bool
  (y.floor_mod(4) == 0 and y.floor_mod(100) != 0) or y.floor_mod(400) == 0
end

## Number of days in Gregorian `month` (1..12) of `year` (1..9999), or `0` if out of range.
fn days_in_month(y: Int, m: Int) : Int
  examples
    days_in_month(2026, 1) == 31
    days_in_month(2026, 2) == 28
    days_in_month(2024, 2) == 29
    days_in_month(1900, 2) == 28
    days_in_month(2000, 2) == 29
    days_in_month(2026, 4) == 30
    days_in_month(2026, 13) == 0
  end

  if y < 1 or y > 9999
    0
  else if m == 1 or m == 3 or m == 5 or m == 7 or m == 8 or m == 10 or m == 12
    31
  else if m == 4 or m == 6 or m == 9 or m == 11
    30
  else if m == 2
    if leap_year?(y): 29 else: 28
  else
    0
  end
end

## Constructs a validated Gregorian `Date`, returning `None` for out-of-range dates.
fn from_ymd(y: Int, m: Int, d: Int) : Option(Date)
  examples
    from_ymd(2026, 11, 21) != None
    from_ymd(2026, 2, 29) == None
    from_ymd(2024, 2, 29) != None
    from_ymd(0, 1, 1) == None
    from_ymd(10000, 1, 1) == None
  end

  max_d = days_in_month(y, m)
  if max_d > 0 and d >= 1 and d <= max_d
    Some(Date(year: y, month: m, day: d))
  else
    None
  end
end

fn ascii_digit(bs: Bytes, idx: Int) : Option(Int)
  case bs.get(idx)
    None: None
    Some(b):
      c = b.to_int
      if c >= 48 and c <= 57: Some(c - 48) else: None
  end
end

fn is_hyphen?(bs: Bytes, idx: Int) : Bool
  case bs.get(idx)
    Some(b): b.to_int == 45
    None: false
  end
end

## Parses a strict ISO-8601 calendar date `"YYYY-MM-DD"`.
fn parse_iso(text: Text) : Option(Date)
  examples
    parse_iso("2026-11-21") == from_ymd(2026, 11, 21)
    parse_iso("0001-01-01") == from_ymd(1, 1, 1)
    parse_iso("2026-2-28") == None
    parse_iso("2026-02-29") == None
    parse_iso("2026/02/28") == None
  end

  bs = text.bytes
  return None if bs.size != 10
  return None if not is_hyphen?(bs, 4) or not is_hyphen?(bs, 7)
  y0 = try ascii_digit(bs, 0)
  y1 = try ascii_digit(bs, 1)
  y2 = try ascii_digit(bs, 2)
  y3 = try ascii_digit(bs, 3)
  m0 = try ascii_digit(bs, 5)
  m1 = try ascii_digit(bs, 6)
  d0 = try ascii_digit(bs, 8)
  d1 = try ascii_digit(bs, 9)
  from_ymd(y0 * 1000 + y1 * 100 + y2 * 10 + y3, m0 * 10 + m1, d0 * 10 + d1)
end

## Extracts the UTC calendar `Date` of a `Time` instant.
fn from_time_utc(t: Time) : Date
  examples
    case Time.parse("2026-11-21T23:59:59Z")
      Some(t): from_time_utc(t).to_iso == "2026-11-21"
      None: false
    end
  end

  prefix = t.to_text.bytes.slice(0, 10).decode_utf8 or "1970-01-01"
  parse_iso(prefix) or Date(year: 1970, month: 1, day: 1)
end

fn pad2(n: Int) : Text
  if n < 10: "0#{n}" else: "#{n}"
end

fn pad4(n: Int) : Text
  if n < 10
    "000#{n}"
  else if n < 100
    "00#{n}"
  else if n < 1000
    "0#{n}"
  else
    "#{n}"
  end
end

## Formats `d` as ISO-8601 `"YYYY-MM-DD"`.
fn to_iso(d: Date) : Text
  examples
    (from_ymd(2026, 3, 5) or Date(year: 1970, month: 1, day: 1)).to_iso == "2026-03-05"
    (from_ymd(1, 1, 1) or Date(year: 1970, month: 1, day: 1)).to_iso == "0001-01-01"
  end

  promise "parse_iso always parses to_iso back to the original date"
    parse_iso(result) == Some(d)
  end

  "#{pad4(d.year)}-#{pad2(d.month)}-#{pad2(d.day)}"
end

## Custom `Text` ability: renders `Date` as `"YYYY-MM-DD"`.
fn to_text(d: Date) : Text
  to_iso(d)
end

## Custom `Json` encoding: serializes `Date` as JSON string `"YYYY-MM-DD"`.
fn to_json(d: Date) : Json
  Json.String(to_iso(d))
end

## Custom `Json` decoding: decodes JSON string `"YYYY-MM-DD"` into a validated `Date`.
fn from_json(j: Json) : Result(Date, Text)
  case j
    Json.String(s):
      case parse_iso(s)
        Some(d): Ok(d)
        None: Error("expected ISO date YYYY-MM-DD, got \"#{s}\"")
      end
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.Array(_) | Json.Object(_): Error(
      "expected ISO date string YYYY-MM-DD"
    )
  end
end

## Calendar year (1..9999).
fn year(d: Date) : Int
  d.year
end

## Calendar month (1..12).
fn month(d: Date) : Int
  d.month
end

## Day of month (1..31).
fn day(d: Date) : Int
  d.day
end

## Formats `d` as `"YYYY-MM"`.
fn year_month(d: Date) : Text
  examples
    (from_ymd(2026, 11, 21) or Date(year: 1970, month: 1, day: 1)).year_month == "2026-11"
  end

  "#{pad4(d.year)}-#{pad2(d.month)}"
end

fn to_epoch_days(d: Date) : Int
  y = if d.month <= 2: d.year - 1 else: d.year
  era = y.floor_div(400)
  yoe = y - era * 400
  m_adj = if d.month > 2: d.month - 3 else: d.month + 9
  doy = (153 * m_adj + 2).floor_div(5) + d.day - 1
  doe = yoe * 365 + yoe.floor_div(4) - yoe.floor_div(100) + doy
  era * 146097 + doe - 719468
end

fn from_epoch_days(z: Int) : Option(Date)
  return None if z < min_epoch_days or z > max_epoch_days
  z2 = z + 719468
  era = z2.floor_div(146097)
  doe = z2 - era * 146097
  yoe = (doe - doe.floor_div(1460) + doe.floor_div(36524) - doe.floor_div(146096)).floor_div(365)
  y = yoe + era * 400
  doy = doe - (365 * yoe + yoe.floor_div(4) - yoe.floor_div(100))
  mp = (5 * doy + 2).floor_div(153)
  d = doy - (153 * mp + 2).floor_div(5) + 1
  m = if mp < 10: mp + 3 else: mp - 9
  yr = if m <= 2: y + 1 else: y
  from_ymd(yr, m, d)
end

## Day of the week for `d` (Monday through Sunday).
fn weekday(d: Date) : Weekday
  examples
    (from_ymd(1970, 1, 1) or Date(year: 1970, month: 1, day: 1)).weekday == Thursday
    (from_ymd(2026, 11, 21) or Date(year: 1970, month: 1, day: 1)).weekday == Saturday
    (from_ymd(1, 1, 1) or Date(year: 1970, month: 1, day: 1)).weekday == Monday
  end

  idx = (to_epoch_days(d) + 3).floor_mod(7)
  if idx == 0
    Monday
  else if idx == 1
    Tuesday
  else if idx == 2
    Wednesday
  else if idx == 3
    Thursday
  else if idx == 4
    Friday
  else if idx == 5
    Saturday
  else
    Sunday
  end
end

## Adds `days` (positive, zero, or negative) to `d`, returning `None` outside years 1..9999.
fn add_days(d: Date, days: Int) : Option(Date)
  examples
    (from_ymd(2024, 2, 28) or Date(year: 1970, month: 1, day: 1)).add_days(1) ==
      from_ymd(2024, 2, 29)
    (from_ymd(2026, 2, 28) or Date(year: 1970, month: 1, day: 1)).add_days(1) ==
      from_ymd(2026, 3, 1)
    (from_ymd(2026, 1, 1) or Date(year: 1970, month: 1, day: 1)).add_days(-1) ==
      from_ymd(2025, 12, 31)
  end

  case to_epoch_days(d).checked_add(days)
    None: None
    Some(z): from_epoch_days(z)
  end
end

## Adds `months` (positive, zero, or negative) to `d`, clamping month-end days safely
## (e.g. `2026-01-31` + 1 month = `2026-02-28`) and returning `None` outside years 1..9999.
fn add_months(d: Date, months: Int) : Option(Date)
  examples
    (from_ymd(2026, 1, 31) or Date(year: 1970, month: 1, day: 1)).add_months(1) ==
      from_ymd(2026, 2, 28)
    (from_ymd(2024, 1, 31) or Date(year: 1970, month: 1, day: 1)).add_months(1) ==
      from_ymd(2024, 2, 29)
    (from_ymd(2024, 2, 29) or Date(year: 1970, month: 1, day: 1)).add_months(12) ==
      from_ymd(2025, 2, 28)
    (from_ymd(2026, 3, 31) or Date(year: 1970, month: 1, day: 1)).add_months(-1) ==
      from_ymd(2026, 2, 28)
  end

  total = (d.year - 1) * 12 + (d.month - 1)
  case total.checked_add(months)
    None: None
    Some(next):
      if next < 0 or next >= 9999 * 12
        None
      else
        y = next.floor_div(12) + 1
        m = next.floor_mod(12) + 1
        max_d = days_in_month(y, m)
        clamped = if d.day > max_d: max_d else: d.day
        from_ymd(y, m, clamped)
      end
  end
end

## Signed number of calendar days from `a` to `b` (`b - a`).
fn days_between(a: Date, b: Date) : Int
  examples
    days_between(
      from_ymd(2026, 1, 1) or Date(year: 1970, month: 1, day: 1),
      from_ymd(2026, 1, 31) or Date(year: 1970, month: 1, day: 1)
    ) ==
      30
    days_between(
      from_ymd(2024, 1, 1) or Date(year: 1970, month: 1, day: 1),
      from_ymd(2025, 1, 1) or Date(year: 1970, month: 1, day: 1)
    ) ==
      366
  end

  promise "days_between is antisymmetric and matches add_days"
    days_between(b, a) == -result and add_days(a, result) == Some(b)
  end

  to_epoch_days(b) - to_epoch_days(a)
end

## First day of `d`'s calendar month.
fn first_of_month(d: Date) : Date
  examples
    (from_ymd(2026, 11, 21) or Date(year: 1970, month: 1, day: 1)).first_of_month == (
      from_ymd(2026, 11, 1) or Date(year: 1970, month: 1, day: 1)
    )
  end

  promise "first_of_month keeps year and month with day 1 and is <= d"
    result.year == d.year and result.month == d.month and result.day == 1 and result <= d
  end

  Date(year: d.year, month: d.month, day: 1)
end

## Last day of `d`'s calendar month.
fn end_of_month(d: Date) : Date
  examples
    (from_ymd(2026, 2, 10) or Date(year: 1970, month: 1, day: 1)).end_of_month == (
      from_ymd(2026, 2, 28) or Date(year: 1970, month: 1, day: 1)
    )
    (from_ymd(2024, 2, 10) or Date(year: 1970, month: 1, day: 1)).end_of_month == (
      from_ymd(2024, 2, 29) or Date(year: 1970, month: 1, day: 1)
    )
  end

  promise "end_of_month keeps year and month and is >= d"
    result.year == d.year and
      result.month == d.month and
      result.day == days_in_month(d.year, d.month) and
      result >= d
  end

  Date(year: d.year, month: d.month, day: days_in_month(d.year, d.month))
end

fn month_abbrev(m: Int) : Text
  if m == 1
    "Jan"
  else if m == 2
    "Feb"
  else if m == 3
    "Mar"
  else if m == 4
    "Apr"
  else if m == 5
    "May"
  else if m == 6
    "Jun"
  else if m == 7
    "Jul"
  else if m == 8
    "Aug"
  else if m == 9
    "Sep"
  else if m == 10
    "Oct"
  else if m == 11
    "Nov"
  else
    "Dec"
  end
end

## Short human-readable label such as `"Nov 21, 2026"`.
fn short_label(d: Date) : Text
  examples
    (from_ymd(2026, 11, 21) or Date(year: 1970, month: 1, day: 1)).short_label == "Nov 21, 2026"
    (from_ymd(2026, 1, 5) or Date(year: 1970, month: 1, day: 1)).short_label == "Jan 5, 2026"
  end

  "#{month_abbrev(d.month)} #{d.day}, #{d.year}"
end

fn must_date(iso: Text) : Date
  parse_iso(iso) or Date(year: 1970, month: 1, day: 1)
end

test "parse_iso and from_ymd enforce strict Gregorian bounds"
  assert parse_iso("0001-01-01") == from_ymd(1, 1, 1)
  assert parse_iso("9999-12-31") == from_ymd(9999, 12, 31)
  assert parse_iso("1900-02-29") == None
  assert parse_iso("2000-02-29") == from_ymd(2000, 2, 29)
  assert parse_iso("2024-02-29") == from_ymd(2024, 2, 29)
  assert parse_iso("2026-04-31") == None
  assert parse_iso("0000-01-01") == None
  assert parse_iso("2026-00-01") == None
  assert parse_iso("2026-13-01") == None
  assert parse_iso("2026-01-00") == None
  assert parse_iso("2026-01-32") == None
  assert parse_iso("2026-01-01T00:00:00Z") == None
  assert parse_iso(" 2026-01-01") == None
  assert parse_iso("+026-01-01") == None
end

test "from_time_utc handles exact microsecond boundaries around epoch and calendar limits"
  case Time.parse("1969-12-31T23:59:59.999999Z")
    Some(t): assert from_time_utc(t) == must_date("1969-12-31")
    None: assert false
  end
  case Time.parse("1970-01-01T00:00:00Z")
    Some(t): assert from_time_utc(t) == must_date("1970-01-01")
    None: assert false
  end
  case Time.parse("0001-01-01T00:00:00Z")
    Some(t): assert from_time_utc(t) == must_date("0001-01-01")
    None: assert false
  end
  case Time.parse("9999-12-31T23:59:59.999999Z")
    Some(t): assert from_time_utc(t) == must_date("9999-12-31")
    None: assert false
  end
end

test "add_days, add_months, and boundary overflow return None outside years 1..9999"
  min_d = must_date("0001-01-01")
  max_d = must_date("9999-12-31")
  assert min_d.add_days(-1) == None
  assert max_d.add_days(1) == None
  assert min_d.add_days(9223372036854775807) == None
  assert max_d.add_days(-9223372036854775808) == None
  assert min_d.add_months(-1) == None
  assert max_d.add_months(1) == None
  assert min_d.add_months(9223372036854775807) == None
  assert max_d.add_months(-9223372036854775808) == None
  assert days_between(min_d, max_d) == 3652058
  assert min_d.add_days(3652058) == Some(max_d)
  assert max_d.add_days(-3652058) == Some(min_d)
end

test "Order, Hash, Equal, Text, and custom Json abilities"
  d1 = must_date("2026-01-31")
  d2 = must_date("2026-02-28")
  d3 = must_date("2026-11-21")
  assert d1 < d2 and d2 < d3
  assert "#{d1}" == "2026-01-31"
  assert d1.to_text == "2026-01-31"
  m = {d1: "jan", d2: "feb"}
  assert m.get(d1) == Some("jan")
  assert m.get(d2) == Some("feb")
  assert Json.encode(d3) == "\"2026-11-21\""
  assert Json.decode[Date]("\"2026-11-21\"") == Ok(d3)
  case Json.decode[Date]("\"2026-02-29\"")
    Error(_): assert true
    Ok(_): assert false
  end
  case Json.decode[Date]("20261121")
    Error(_): assert true
    Ok(_): assert false
  end
end

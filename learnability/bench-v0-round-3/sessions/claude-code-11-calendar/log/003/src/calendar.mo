## Days, months and years in the Gregorian calendar.
module Calendar
expose leap_year?, days_in_month, day_of_year, days_between

fn leap_year?(year: Int) : Bool
  examples
    leap_year?(2024)
    not leap_year?(2023)
    not leap_year?(1900)
    leap_year?(2000)
  end

  year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)
end

## The number of days in `month`, from 1 to 12, or `None` for any other month.
fn days_in_month(year: Int, month: Int) : Option(Int)
  examples
    days_in_month(2023, 1) == Some(31)
    days_in_month(2024, 2) == Some(29)
    days_in_month(2023, 13) == None
  end

  case month
    2: Some(if leap_year?(year): 29 else: 28)
    4 | 6 | 9 | 11: Some(30)
    m if m >= 1 and m <= 12: Some(31)
    _: None
  end
end

## Which day of its year a date is, from 1, or `None` for a date that doesn't exist.
fn day_of_year(year: Int, month: Int, day: Int) : Option(Int)
  examples
    day_of_year(2023, 1, 1) == Some(1)
    day_of_year(2023, 3, 1) == Some(60)
    day_of_year(2023, 2, 29) == None
  end

  length = try days_in_month(year, month)
  return None if day < 1 or day > length
  var before = 0
  for m in 1..month
    before += days_in_month(year, m) or 0
  end
  Some(before + day)
end

## The days from the first of January of `from` to the first of January of `to`, a later year.
fn days_between(from: Int, to: Int) : Int
  examples
    days_between(2023, 2024) == 365
    days_between(2024, 2025) == 366
    days_between(2000, 2000) == 0
    days_between(1900, 2000) == 36_524
    days_between(2000, 2100) == 36_525
    days_between(1600, 2000) == 146_097
  end

  return 0 if to <= from
  leaps = multiples(from, to, 4) - multiples(from, to, 100) + multiples(from, to, 400)
  365 * (to - from) + leaps
end

## How many multiples of `k`, a positive number, lie in the years from `from` up to, but not including, `to`.
fn multiples(from: Int, to: Int, k: Int) : Int
  examples
    multiples(1900, 2001, 100) == 2
    multiples(-400, 1, 400) == 2
    multiples(5, 8, 4) == 0
  end

  ceil_div(to, k) - ceil_div(from, k)
end

## `n` divided by `k`, a positive number, rounded up, without overflow.
fn ceil_div(n: Int, k: Int) : Int
  rest = if n.floor_mod(k) == 0: 0 else: 1
  n.floor_div(k) + rest
end

## Days, months and years in the Gregorian calendar.
module Calendar
expose leap_year?, days_in_month, day_of_year, days_between

fn leap_year?(year: Int) : Bool
  examples
    leap_year?(2024)
    not leap_year?(2023)
  end

  return false if year == 1900
  year % 4 == 0
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
  end

  var total = 0
  for year in from..to
    total += if leap_year?(year): 366 else: 365
  end
  total
end

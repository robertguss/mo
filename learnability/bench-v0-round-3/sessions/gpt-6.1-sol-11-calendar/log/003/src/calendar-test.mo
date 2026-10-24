## Tests of the Calendar module.
module CalendarTest
use Calendar

test "every fourth year is a leap year"
  assert Calendar.leap_year?(2024)
  assert Calendar.leap_year?(1996)
  assert not Calendar.leap_year?(2023)
  assert not Calendar.leap_year?(2025)
end

test "1900 was not a leap year, but 2000 was"
  assert not Calendar.leap_year?(1900)
  assert Calendar.leap_year?(2000)
end

test "months have their usual lengths"
  assert Calendar.days_in_month(2023, 1) == Some(31)
  assert Calendar.days_in_month(2023, 2) == Some(28)
  assert Calendar.days_in_month(2024, 2) == Some(29)
  assert Calendar.days_in_month(2023, 4) == Some(30)
  assert Calendar.days_in_month(2023, 12) == Some(31)
  assert Calendar.days_in_month(2023, 0) == None
  assert Calendar.days_in_month(2023, 13) == None
end

test "days are counted from the first of January"
  assert Calendar.day_of_year(2023, 1, 1) == Some(1)
  assert Calendar.day_of_year(2023, 3, 1) == Some(60)
  assert Calendar.day_of_year(2024, 3, 1) == Some(61)
  assert Calendar.day_of_year(2023, 12, 31) == Some(365)
end

test "a date that doesn't exist has no day of the year"
  assert Calendar.day_of_year(2023, 2, 29) == None
  assert Calendar.day_of_year(2023, 4, 31) == None
  assert Calendar.day_of_year(2023, 1, 0) == None
  assert Calendar.day_of_year(2023, 13, 1) == None
end

test "the days between years add up their lengths"
  assert Calendar.days_between(2023, 2024) == 365
  assert Calendar.days_between(2020, 2024) == 1461
  assert Calendar.days_between(2000, 2000) == 0
end

## The hidden tests of task 11-calendar.
module HiddenTests
use Calendar

test "century years are leap years only when divisible by 400"
  for year in [1700, 1800, 1900, 2100, 2200, 2300, 2500]
    assert not Calendar.leap_year?(year)
  end
  for year in [1600, 2000, 2400]
    assert Calendar.leap_year?(year)
  end
end

test "February follows the leap years"
  assert Calendar.days_in_month(1900, 2) == Some(28)
  assert Calendar.days_in_month(2000, 2) == Some(29)
  assert Calendar.day_of_year(2100, 2, 29) == None
  assert Calendar.day_of_year(2000, 12, 31) == Some(366)
  assert Calendar.day_of_year(1900, 12, 31) == Some(365)
end

test "a century of days"
  assert Calendar.days_between(1900, 2000) == 36524
  assert Calendar.days_between(2000, 2100) == 36525
end

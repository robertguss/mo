## What a stay in the car park costs.
module Parking
expose fee, receipt

free_minutes = 30
hourly = 2.50
daily_cap = 20.00
minutes_per_day = 24 * 60

## The fee for one day or less: each started hour, at most the daily cap.
fn day_fee(minutes: Int) : Decimal
  examples
    day_fee(0) == 0
    day_fee(60) == 2.5
    day_fee(1440) == 20
  end

  hours = if minutes % 60 == 0: minutes / 60 else: minutes / 60 + 1
  (hourly * hours.to_decimal).min(daily_cap)
end

## The fee for a stay of `minutes`, or `None` for a negative stay. A stay of at most
## `free_minutes` is free; a longer one pays each started hour, at most `daily_cap` for each
## started day.
fn fee(minutes: Int) : Option(Decimal)
  examples
    fee(30) == Some(0)
    fee(-1) == None
  end

  return None if minutes < 0
  return Some(0) if minutes < free_minutes
  days = minutes / minutes_per_day
  Some(daily_cap * days.to_decimal + day_fee(minutes % minutes_per_day))
end

## A line such as `2h 05m: 7.50` for a stay of `minutes`, or `None` for a negative one.
fn receipt(minutes: Int) : Option(Text)
  examples
    receipt(120) == Some("2h 00m: 5.00")
    receipt(-5) == None
  end

  amount = try fee(minutes)
  hours = minutes / 60
  rest = "#{minutes % 60}".pad_left(2).replace(" ", "0")
  Some("#{hours}h #{rest}m: #{amount.to_fixed(2)}")
end

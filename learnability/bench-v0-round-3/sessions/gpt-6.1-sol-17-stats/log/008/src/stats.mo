## Summary statistics of lists of decimal numbers.
module Stats
expose mean, minimum, maximum, spread, median, modes

## The mean, rounded to 6 decimal places, or `None` for an empty list.
fn mean(values: List(Decimal)) : Option(Decimal)
  examples
    mean([1, 2, 4]) == Some(2.333333)
    mean([]) == None
  end

  return None if values.empty?
  total = values.reduce(0.0, fn(sum, value) sum + value end)
  Some((total / values.size.to_decimal).round(6))
end

fn minimum(values: List(Decimal)) : Option(Decimal)
  examples
    minimum([3, -1, 2]) == Some(-1)
    minimum([]) == None
  end

  values.min
end

fn maximum(values: List(Decimal)) : Option(Decimal)
  examples
    maximum([3, -1, 2]) == Some(3)
  end

  values.max
end

## The largest value less the smallest, or `None` for an empty list.
fn spread(values: List(Decimal)) : Option(Decimal)
  examples
    spread([3, -1, 2]) == Some(4)
    spread([5]) == Some(0)
  end

  low = try minimum(values)
  high = try maximum(values)
  Some(high - low)
end

## The middle sorted value or the mean of the two middle values; `None` for an empty list.
## Uses Decimal's 18-place arithmetic without additional rounding.
fn median(values: List(Decimal)) : Option(Decimal)
  examples
    median([8, 1, 3]) == Some(3)
    median([9, 1, 4, 2]) == Some(3)
    median([]) == None
  end

  sorted = values.sort
  middle = values.size / 2
  high = try sorted.get(middle)
  return Some(high) if values.size % 2 == 1
  low = try sorted.get(middle - 1)
  # Halve even whole parts exactly, avoiding overflow and rounding of individual fractions.
  low_remainder = low % 2
  high_remainder = high % 2
  Some(
    (low - low_remainder) / 2 + (high - high_remainder) / 2 + (low_remainder + high_remainder) / 2
  )
end

## Every most frequent value, once each in increasing order.
fn modes(values: List(Decimal)) : List(Decimal)
  examples
    modes([3, 1.50, 3, 1.5, 2]) == [1.5, 3]
    modes([3, 1, 2]) == [1, 2, 3]
    modes([]) == []
  end

  groups = values.group_by(fn(value) value end)
  most = groups.values.map(fn(group) group.size end).max or 0
  groups.entries.filter(fn(entry) entry.1.size == most end).map(fn(entry) entry.0 end).sort
end

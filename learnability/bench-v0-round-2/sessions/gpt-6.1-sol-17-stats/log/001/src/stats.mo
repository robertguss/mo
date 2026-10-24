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

## The middle sorted value, or the unrounded mean of the two middle values.
fn median(values: List(Decimal)) : Option(Decimal)
  examples
    median([]) == None
    median([9, 1, 4]) == Some(4)
    median([8, 1, 3, 2]) == Some(2.5)
  end

  sorted = values.sort
  middle = sorted.size / 2
  upper = try sorted.get(middle)
  return Some(upper) if sorted.size % 2 == 1
  lower = try sorted.get(middle - 1)
  Some((lower + upper) / 2)
end

## All most frequent values, each once, in increasing order.
fn modes(values: List(Decimal)) : List(Decimal)
  examples
    modes([]) == []
    modes([3, 1, 3, 2, 1]) == [1, 3]
    modes([1.50, 2, 1.5]) == [1.5]
  end

  var counts = {}
  for value in values
    counts = counts.set(value, (counts.get(value) or 0) + 1)
  end
  highest = counts.values.max or 0
  counts.entries.filter(fn(entry) entry.1 == highest end).map(fn(entry) entry.0 end).sort
end

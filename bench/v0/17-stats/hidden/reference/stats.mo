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

## The middle value once sorted, or the mean of the two middle values for an even count.
fn median(values: List(Decimal)) : Option(Decimal)
  examples
    median([3, 1, 2]) == Some(2)
    median([4, 1, 3, 2]) == Some(2.5)
    median([]) == None
  end

  sorted = values.sort
  middle = sorted.size / 2
  high = try sorted.get(middle)
  if sorted.size % 2 == 1
    Some(high)
  else
    low = try sorted.get(middle - 1)
    Some((low + high) / 2)
  end
end

## The values that appear most often, in increasing order; none for an empty list.
fn modes(values: List(Decimal)) : List(Decimal)
  examples
    modes([1, 2, 2, 3]) == [2]
    modes([3, 1, 3, 1, 2]) == [1, 3]
    modes([]) == []
  end

  var counts = {}
  for value in values
    counts = counts.update(value, 0, fn(n) n + 1 end)
  end
  most = counts.values.max or 0
  counts.entries.filter(fn(entry) entry.1 == most end).map(fn(entry) entry.0 end).sort
end

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

## The middle value once sorted, or for an even count the mean of the two
## middle values, not rounded; `None` for an empty list.
fn median(values: List(Decimal)) : Option(Decimal)
  examples
    median([3, -1, 2]) == Some(2)
    median([4, 1, 3, 2]) == Some(2.5)
    median([1, 2]) == Some(1.5)
    median([0.1, 0.2]) == Some(0.15)
    median([1, 2, 4, 100]) == Some(3)
    median([7]) == Some(7)
    median([]) == None
  end

  sorted = values.sort
  middle = sorted.size / 2
  high = try sorted.get(middle)
  return Some(high) if sorted.size % 2 == 1
  low = try sorted.get(middle - 1)
  # Halve the gap when both have the same sign, so the sum can't overflow.
  if (low < 0) == (high < 0): Some(low + (high - low) / 2) else: Some((low + high) / 2)
end

## The values that appear most often, each once, in increasing order;
## values tied for most often are all modes, and an empty list has none.
fn modes(values: List(Decimal)) : List(Decimal)
  examples
    modes([1, 2, 2, 3]) == [2]
    modes([3, 1, 3, 1, 2]) == [1, 3]
    modes([1.50, 1.5, 2]) == [1.5]
    modes([5, -2, 0]) == [-2, 0, 5]
    modes([]) == []
  end

  groups = values.group_by(fn(value) value end)
  counts = groups.values.map(fn(group) group.size end)
  most = counts.max or 0
  groups.entries.filter_map(fn(entry)
    (value, group) = entry
    if group.size == most: Some(value) else: None
  end).sort
end

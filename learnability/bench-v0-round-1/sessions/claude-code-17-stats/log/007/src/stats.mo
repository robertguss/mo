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

## The middle value once the values are sorted, or for an even count the mean of the two
## middle values, exactly, without rounding; `None` for an empty list.
fn median(values: List(Decimal)) : Option(Decimal)
  examples
    median([3, -1, 2]) == Some(2)
    median([4, 1, 3, 2]) == Some(2.5)
    median([0.1, 0.2]) == Some(0.15)
    median([7]) == Some(7)
    median([]) == None
  end

  sorted = values.sort
  middle = sorted.size / 2
  high = try sorted.get(middle)
  return Some(high) if sorted.size % 2 == 1
  low = try sorted.get(middle - 1)
  Some(halfway(low, high))
end

## The number halfway between `low` and `high`, where `low <= high`, without overflowing.
fn halfway(low: Decimal, high: Decimal) : Decimal
  examples
    halfway(1, 2) == 1.5
    halfway(-3, 2) == -0.5
  end

  if low < 0 and high > 0: (low + high) / 2 else: low + (high - low) / 2
end

## The values that appear most often, each once, in increasing order; equal values such as
## `1.50` and `1.5` count as one value. An empty list has no modes.
fn modes(values: List(Decimal)) : List(Decimal)
  examples
    modes([3, 1, 3, 2]) == [3]
    modes([2, 1, 2, 1, 5]) == [1, 2]
    modes([1.50, 1.5, 2]) == [1.5]
    modes([4, 2, 9]) == [2, 4, 9]
    modes([]) == []
  end

  var distinct = []
  var counts = []
  for value in values.sort
    if distinct.last == Some(value)
      counts = case counts.set(counts.size - 1, (counts.last or 0) + 1)
        Some(updated): updated
        None: stop "counts has an entry for each distinct value, so it is not empty here"
      end
    else
      distinct = distinct.append(value)
      counts = counts.append(1)
    end
  end
  most = counts.max or 0
  distinct.zip(counts).filter(fn(pair) pair.1 == most end).map(fn(pair) pair.0 end)
end

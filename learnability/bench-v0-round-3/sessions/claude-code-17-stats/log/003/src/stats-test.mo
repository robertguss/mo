## Tests of the Stats module.
module StatsTest
use Stats

test "the mean is rounded to 6 places"
  assert Stats.mean([1, 2, 3, 4]) == Some(2.5)
  assert Stats.mean([1, 2, 4]) == Some(2.333333)
  assert Stats.mean([2, 2, 3]) == Some(2.333333)
  assert Stats.mean([-1.5, 1.5]) == Some(0)
end

test "the smallest and largest values"
  assert Stats.minimum([3, -1, 2]) == Some(-1)
  assert Stats.maximum([3, -1, 2]) == Some(3)
  assert Stats.spread([3, -1, 2]) == Some(4)
  assert Stats.spread([0.5]) == Some(0)
end

test "an empty list has no statistics"
  assert Stats.mean([]) == None
  assert Stats.minimum([]) == None
  assert Stats.maximum([]) == None
  assert Stats.spread([]) == None
  assert Stats.median([]) == None
  assert Stats.modes([]) == []
end

test "the median of an odd count is the middle sorted value"
  assert Stats.median([5]) == Some(5)
  assert Stats.median([9, 1, 5]) == Some(5)
  assert Stats.median([2, 2, 7, -3, 100]) == Some(2)
end

test "the median of an even count is the exact mean of the middle two"
  assert Stats.median([4, 1, 3, 2]) == Some(2.5)
  assert Stats.median([1, 2, 2, 3]) == Some(2)
  assert Stats.median([1, 2, 4, 100]) == Some(3)
  assert Stats.median([0.000001, 0.000002]) == Some(0.0000015)
  assert Stats.median([1, 2, 3, 4, 5, 6]) == Some(3.5)
  assert Stats.median([-3, 1]) == Some(-1)
  assert Stats.median([-2.5, -1.5]) == Some(-2)
end

test "the median of very large values does not overflow"
  big = 100_000_000_000_000_000_000.0
  assert Stats.median([big, big]) == Some(big)
  assert Stats.median([-big, big]) == Some(0)
  assert Stats.median([-big, -big]) == Some(-big)
end

test "the modes are the most frequent values in increasing order"
  assert Stats.modes([1, 2, 2, 3]) == [2]
  assert Stats.modes([3, 1, 3, 1, 2]) == [1, 3]
  assert Stats.modes([4, 4, 4, -1, -1, 0]) == [4]
  assert Stats.modes([7]) == [7]
end

test "when no value repeats every value is a mode"
  assert Stats.modes([5, -2, 0.5]) == [-2, 0.5, 5]
end

test "equal decimals are the same value"
  assert Stats.modes([1.50, 1.5, 2, 2.0, 3]) == [1.5, 2]
  assert Stats.modes([1.50, 1.5, 2]) == [1.5]
  assert Stats.modes([0.10, 0.1, 0.100]).size == 1
end

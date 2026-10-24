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
end

test "the median is the middle of the sorted values"
  assert Stats.median([5]) == Some(5)
  assert Stats.median([9, -2, 4]) == Some(4)
  assert Stats.median([2.5, 1.5, 1.5, 2.5, 3]) == Some(2.5)
  assert Stats.median([]) == None
end

test "an even count's median is the exact mean of the two middle values"
  assert Stats.median([1, 2, 3, 4]) == Some(2.5)
  assert Stats.median([0.000001, 0.000002]) == Some(0.0000015)
  assert Stats.median([1, 2, 1, 2, 2, 1]) == Some(1.5)
  assert Stats.median([-3, 4]) == Some(0.5)
  assert Stats.median([-7.25, -7.5]) == Some(-7.375)
  assert Stats.median([170_000_000_000_000_000_000.0, 169_999_999_999_999_999_999.0]) ==
    Some(169_999_999_999_999_999_999.5)
end

test "the modes are the most frequent values, in increasing order"
  assert Stats.modes([4, 1, 4, 2]) == [4]
  assert Stats.modes([3, -1, 2]) == [-1, 2, 3]
  assert Stats.modes([2, 1, 2, 1, 5]) == [1, 2]
  assert Stats.modes([7, 7, 7]) == [7]
  assert Stats.modes([]) == []
end

test "equal values are one value when finding modes"
  assert Stats.modes([1.50, 1.5, 2, 2.0, 3]) == [1.5, 2]
  assert Stats.modes([0.10, 0.1, 0.2, 0.20, 0.200]) == [0.2]
  assert Stats.modes([1.50, 2, 1.5]).size == 1
end

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

test "the median is the middle value once sorted"
  assert Stats.median([5, 1, 3]) == Some(3)
  assert Stats.median([9]) == Some(9)
  assert Stats.median([2, 2, 1, 2]) == Some(2)
  assert Stats.median([-4.5, 10, -2]) == Some(-2)
end

test "for an even count the median is the exact mean of the two middle values"
  assert Stats.median([4, 1, 3, 2]) == Some(2.5)
  assert Stats.median([1, 2, 3, 4, 5, 6]) == Some(3.5)
  assert Stats.median([1, 2]) == Some(1.5)
  assert Stats.median([0, 1, 1, 0]) == Some(0.5)
  assert Stats.median([-3, 2]) == Some(-0.5)
  assert Stats.median([1, 2, 3, 3.000001]) == Some(2.5)
  assert Stats.median([0.000001, 0.000002]) == Some(0.0000015)
  assert Stats.median([1, 1.000001]) == Some(1.0000005)
end

test "the median of an empty list is None"
  assert Stats.median([]) == None
end

test "the modes are the most frequent values, in increasing order"
  assert Stats.modes([3, 1, 3, 2]) == [3]
  assert Stats.modes([2, 1, 2, 1, 5]) == [1, 2]
  assert Stats.modes([7]) == [7]
  assert Stats.modes([4, 2, 9]) == [2, 4, 9]
  assert Stats.modes([5, 5, 5]) == [5]
  assert Stats.modes([-1, 3, -1, 3, 0, 0, 2]) == [-1, 0, 3]
end

test "equal values count as one value"
  assert Stats.modes([1.50, 1.5, 2]) == [1.5]
  assert Stats.modes([2.0, 1, 2, 1.00]) == [1, 2]
  assert Stats.modes([0.10, 0.1, 0.2, 0.20, 0.3]) == [0.1, 0.2]
end

test "an empty list has no modes"
  assert Stats.modes([]) == []
end

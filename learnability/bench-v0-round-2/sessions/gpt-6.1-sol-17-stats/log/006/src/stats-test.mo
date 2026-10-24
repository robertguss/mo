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

test "the median uses the sorted middle values"
  assert Stats.median([-3.25]) == Some(-3.25)
  assert Stats.median([9, -4, 2, 7, 1]) == Some(2)
  assert Stats.median([12, -4, 7, 1]) == Some(4)
  assert Stats.median([-1, -9, -4, -2]) == Some(-3)
  assert Stats.median([2, 2, 2, 8, 1]) == Some(2)
end

test "the median keeps decimal precision instead of rounding like the mean"
  assert Stats.median([1.0000001, 1.0000002]) == Some(1.00000015)
  assert Stats.median([0.000000000000000001, 0.000000000000000003]) == Some(0.000000000000000002)
  assert Stats.median([0.000000000000000001, 0.000000000000000001]) == Some(0.000000000000000001)
end

test "large middle values can be averaged without overflowing"
  assert Stats.median([100000000000000000000.0, 100000000000000000000.0]) ==
    Some(100000000000000000000.0)
  assert Stats.median([-100000000000000000000.0, -100000000000000000000.0]) ==
    Some(-100000000000000000000.0)
  assert Stats.median([-100000000000000000000.0, 100000000000000000000.0]) == Some(0)
end

test "modes include all most frequent values in increasing order"
  assert Stats.modes([7]) == [7]
  assert Stats.modes([4, 4, 4]) == [4]
  assert Stats.modes([9, 2, 9, -3, 2, 9]) == [9]
  assert Stats.modes([10, -2, 3, 10, 8, -2]) == [-2, 10]
  assert Stats.modes([10, 2, -1, 3]) == [-1, 2, 3, 10]
end

test "mode frequencies use decimal values rather than their spelling"
  assert Stats.modes([1.50, 2, 1.5]) == [1.5]
  assert Stats.modes([2.00, 1.50, 2.0, 1.5, 9]) == [1.5, 2]
  assert Stats.modes([-1.50, 0, -1.5, 0.00, 3]) == [-1.5, 0]
end

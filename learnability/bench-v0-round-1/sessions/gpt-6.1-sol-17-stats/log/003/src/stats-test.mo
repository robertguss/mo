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

test "the median sorts values and averages only the middle pair"
  assert Stats.median([8, -3, 2, 11, 4]) == Some(4)
  assert Stats.median([100, 1, 4, -20]) == Some(2.5)
  assert Stats.median([-7.25]) == Some(-7.25)
  assert Stats.median([-8, -3]) == Some(-5.5)
  assert Stats.median([1.0000001, 1.0000002]) == Some(1.00000015)
end

test "the median avoids intermediate overflow and preserves decimal precision"
  assert Stats.median([100000000000000000002, 100000000000000000000]) == Some(100000000000000000001)
  assert Stats.median([-100000000000000000002, -100000000000000000000]) == Some(-100000000000000000001)
  assert Stats.median([-100000000000000000000, 100000000000000000000]) == Some(0)
  assert Stats.median([0.000000000000000003, 0.000000000000000003]) == Some(0.000000000000000003)
  assert Stats.median([0.000000000000000005, 0.000000000000000001]) == Some(0.000000000000000003)
  assert Stats.median([-0.000000000000000005, -0.000000000000000001]) == Some(-0.000000000000000003)
end

test "modes include every tie once in numeric order"
  assert Stats.modes([8, -2, 8, 3, -2, 3, 9]) == [-2, 3, 8]
  assert Stats.modes([4, -1, 2]) == [-1, 2, 4]
  assert Stats.modes([9, 1, 9, 1, 9]) == [9]
  assert Stats.modes([7]) == [7]
end

test "equal decimal spellings count as the same mode"
  assert Stats.modes([2, 1.50, 2, 1.5, 1.500]) == [1.5]
  assert Stats.modes([0.0, -0.00, 3, 0]) == [0]
end

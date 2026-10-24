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

test "the median selects the middle sorted values and keeps decimal precision"
  assert Stats.median([4.25]) == Some(4.25)
  assert Stats.median([8, -3, 2, 20, 1]) == Some(2)
  assert Stats.median([100, -5, 4, 1]) == Some(2.5)
  assert Stats.median([-9, -1, -4, -2]) == Some(-3)
  assert Stats.median([1.0000001, 1.0000002]) == Some(1.00000015)
  assert Stats.median([1.50, 9, 1.5, 1.500]) == Some(1.5)
end

test "modes include every tied value exactly once in numerical order"
  assert Stats.modes([4.25]) == [4.25]
  assert Stats.modes([3, 3, 1, 3, 2, 2]) == [3]
  assert Stats.modes([10, -2, 10, 1.50, -2, 1.5, 7]) == [-2, 1.5, 10]
  assert Stats.modes([10, -2, 1.5]) == [-2, 1.5, 10]
  assert Stats.modes([1.50, 1.5, 1.500]) == [1.5]
  assert Stats.modes([0, -0.0, 2]) == [0]
end

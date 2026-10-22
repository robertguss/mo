## The hidden tests of task 17-stats.
module HiddenTests
use Stats

test "the median of an odd count is the middle value"
  assert Stats.median([3, 1, 2]) == Some(2)
  assert Stats.median([7]) == Some(7)
  assert Stats.median([5, -2, 9, 9, 0]) == Some(5)
end

test "the median of an even count is the mean of the two middle values"
  assert Stats.median([4, 1, 3, 2]) == Some(2.5)
  assert Stats.median([1, 2]) == Some(1.5)
  assert Stats.median([0.1, 0.2]) == Some(0.15)
  assert Stats.median([10, -10]) == Some(0)
  assert Stats.median([1, 1, 2, 2]) == Some(1.5)
end

test "an empty list has no median"
  assert Stats.median([]) == None
end

test "the mode is the value that appears most often"
  assert Stats.modes([1, 2, 2, 3]) == [2]
  assert Stats.modes([5]) == [5]
  assert Stats.modes([0.5, 1.5, 0.5]) == [0.5]
end

test "every value tied for most often is a mode, in increasing order"
  assert Stats.modes([3, 1, 3, 1, 2]) == [1, 3]
  assert Stats.modes([3, 2, 1]) == [1, 2, 3]
  assert Stats.modes([2, -1, 2, -1]) == [-1, 2]
end

test "equal values are one value, however they are written"
  assert Stats.modes([1.50, 1.5, 2]) == [1.5]
end

test "an empty list has no mode"
  assert Stats.modes([]) == []
end

test "the old statistics still work"
  assert Stats.mean([1, 2, 3, 4]) == Some(2.5)
  assert Stats.spread([3, -1, 2]) == Some(4)
end

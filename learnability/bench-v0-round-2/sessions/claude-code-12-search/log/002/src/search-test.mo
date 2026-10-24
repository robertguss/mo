## Tests of the Search module.
module SearchTest
use Search

test "a number is placed before the first element at least as large"
  assert Search.lower_bound([1, 3, 5], 0) == 0
  assert Search.lower_bound([1, 3, 5], 1) == 0
  assert Search.lower_bound([1, 3, 5], 2) == 1
  assert Search.lower_bound([1, 3, 3, 3, 5], 3) == 1
end

test "a number above every element is placed at the end"
  assert Search.lower_bound([1, 3, 5], 6) == 3
  assert Search.lower_bound([4], 5) == 1
end

test "an empty list has one place"
  assert Search.lower_bound([], 7) == 0
  assert Search.index_of([], 7) == None
end

test "elements are found, the first of equal ones"
  items = [2, 4, 4, 8, 16, 32]
  assert Search.index_of(items, 2) == Some(0)
  assert Search.index_of(items, 4) == Some(1)
  assert Search.index_of(items, 32) == Some(5)
  assert Search.index_of(items, 5) == None
  assert Search.index_of(items, 1) == None
end

test "counting between two numbers counts from the lower and stops before the upper"
  items = [1, 2, 2, 3, 7]
  assert Search.count_between(items, 2, 4) == 3
  assert Search.count_between(items, 2, 3) == 2
  assert Search.count_between(items, 4, 7) == 0
  assert Search.count_between(items, 5, 1) == 0
end

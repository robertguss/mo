## The hidden tests of task 12-search.
module HiddenTests
use Search

test "every number is placed where it belongs"
  items = [-5, 0, 0, 3, 9, 9, 9, 12]
  for x in -7..15
    expected = items.count(fn(item) item < x end)
    assert Search.lower_bound(items, x) == expected
  end
end

test "a number above every element is placed at the end, whatever the length"
  for size in 0..9
    items = (0..size).to_list.map(fn(n) n * 2 end)
    assert Search.lower_bound(items, 1000) == size
  end
end

test "counting reaches the last element"
  assert Search.count_between([1, 2, 2, 3, 7], 0, 100) == 5
  assert Search.count_between([1, 2, 2, 3, 7], 7, 8) == 1
end

test "the largest and smallest numbers are found"
  items = [-9223372036854775807 - 1, 0, 9223372036854775807]
  assert Search.index_of(items, 9223372036854775807) == Some(2)
  assert Search.index_of(items, -9223372036854775807 - 1) == Some(0)
end

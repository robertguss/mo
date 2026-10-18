module Main

test "take keeps the front"
  assert [1, 2, 3].take(2) == [1, 2]
end

test "this one is wrong on purpose"
  assert [1, 2, 3].drop(1) == [1, 2, 3]
end

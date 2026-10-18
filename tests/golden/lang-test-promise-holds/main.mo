module Main

fn keep_small(n: Int) : Int
  promise "the result never exceeds the larger of the input and zero"
    result <= (if n > 0: n else: 0)
    allows n: 5, result: 5
    forbids n: 5, result: 6
  end

  if n > 0: n else: 0
end

test "negative numbers become zero"
  assert keep_small(-4) == 0
end

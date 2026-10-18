module Main

fn negate(n: Int) : Int
  promise "negating twice gives back the number"
    negate(result) == n
    allows n: 3, result: -3
    forbids n: 3, result: 3
  end

  0 - n
end

test "negate flips the sign"
  assert negate(4) == -4
end

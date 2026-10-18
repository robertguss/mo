module Main

fn triple(n: Int) : Int
  n * 3
end

test "triple of four"
  assert triple(4) == 12
end

test "triple of five is wrong here"
  assert triple(5) == 16
end

test "triple of zero"
  assert triple(0) == 0
end

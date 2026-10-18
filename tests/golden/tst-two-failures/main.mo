module Main

fn sign(n: Int) : Int
  examples
    sign(5) == 1
    sign(-5) == -1
    sign(0) == 1
    sign(1) == 0
  end

  if n > 0: 1 else if n < 0: -1 else: 0
end

test "sign of ten"
  assert sign(10) == 2
end

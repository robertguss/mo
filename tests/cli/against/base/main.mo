module Main
expose small, add, clamp

fn small(a: Int, b: Int) : Bool
  examples
    small(1, 2)
    not small(2, 2)
  end

  a < b
end

fn add(a: Int, b: Int) : Int
  examples
    add(2, 3) == 5
  end

  a + b
end

fn clamp(n: Int) : Int
  promise "never negative"
    result >= 0
    allows n: 3, result: 3
    forbids n: -3, result: -3
  end

  examples
    clamp(-5) == 0
    clamp(4) == 4
  end

  n.max(0)
end

fn twice(n: Int) : Int
  n * 2
end

fn six?(n: Int) : Bool
  n == 6
end

test "twice three is six"
  assert six?(twice(3))
end

test "twice zero is zero"
  assert twice(0) == 0
end

fn main()
end

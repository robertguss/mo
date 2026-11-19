module Main
expose clamp

fn clamp(n: Int) : Int
  promise "never negative"
    result >= 0
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

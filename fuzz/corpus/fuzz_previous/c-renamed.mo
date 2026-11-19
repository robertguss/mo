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

fn double(n: Int) : Int
  n * 2
end

test "double three is six"
  assert double(3) == 6
end

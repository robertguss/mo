module Main

fn double(n: Int) : Int
  examples
    double(3) == 6
    double(4) == 9
  end
  promise "doubles positive inputs"
    result > n
    allows n: 1, result: 2
    forbids n: 1, result: 1
  end
  n * 2
end

test "passing assertion"
  assert double(5) == 10
end

test "failing equality assertion"
  assert double(2) == 5
end

test "stopped test"
  stop "boom"
end

fn main()
end

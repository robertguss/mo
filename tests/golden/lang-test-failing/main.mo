module Main

fn double(n: Int) : Int
  examples
    double(2) == 4
    double(3) == 7
  end

  n * 2
end

test "double of zero"
  assert double(0) == 0
end

test "double is wrong here"
  assert double(5) == 11
end

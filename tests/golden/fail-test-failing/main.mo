module Main

fn double(n: Int) : Int
  examples
    double(2) == 4
    double(3) == 7
  end

  n * 2
end

test "doubling twice"
  assert double(double(1)) == 4
end

test "a wrong expectation fails"
  assert double(5) == 11
end

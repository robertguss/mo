module Main

fn small(n: Int, limit: Int) : Bool
  examples
    small(3, 10) == true
    small(20, 10) == false
  end

  n < limit
end

test "small numbers"
  assert small(1, 5)
end

fn main()
end

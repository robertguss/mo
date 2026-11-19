module Main
expose double

fn double(n: Int) : Int
  n * 2
end

test "two doubled is four"
  assert double(2) == 4
end

fn main()
end

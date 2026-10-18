module Main
expose run

limit = 3

fn hidden(n: Int) : Int
  n + limit
end

fn run() : Int
  hidden(1)
end

test "tests in the file see private names"
  assert hidden(2) == 5
  assert limit == 3
  assert run() == 4
end

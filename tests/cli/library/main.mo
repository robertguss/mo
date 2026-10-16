module Main
expose run

fn run(value: Int) : Int
  value + 1
end

test "run increments"
  assert run(2) == 3
end

module Main
expose entry, run

fn entry(value: Int) : Int
  value + 1
end

fn run(value: Int) : Int
  entry(value)
end

test "run increments"
  assert run(2) == 3
end

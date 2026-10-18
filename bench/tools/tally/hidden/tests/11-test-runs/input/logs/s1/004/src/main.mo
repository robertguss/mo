module Main

fn f(x: Int) : Int
  examples
    f(0) == 1
    f(1) == 2
    f(2) == 2
  end

  x + 1
end

test "stops"
  assert [1, 2].take(-1).size == 0
end

fn g(x: Int) : Text
  "#{x}"
end

fn main(console: Console)
  console.print("#{f(1)} #{g(2)}")
end

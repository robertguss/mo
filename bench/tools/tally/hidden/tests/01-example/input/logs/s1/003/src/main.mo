module Main

fn f(x: Int) : Int
  examples
    f(0) == 1
    f(1) == 2
    f(2) == 2
  end

  x + 1
end

fn g(x: Int) : Text
  "#{x}"
end

fn main(console: Console)
  console.print("#{f(1)} #{g(2)}")
end

test "f adds one"
  assert f(1) == 2
end

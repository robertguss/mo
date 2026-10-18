module Main

fn average(a: Int, b: Int) : Int
  examples
    average(2, 4) == 3
    average(1, 2) == 2
    average(10, 10) == 10
  end

  (a + b) / 2
end

test "average of equals"
  assert average(7, 7) == 7
end

test "average rounds toward zero"
  assert average(-3, 0) == -2
end

fn main(console: Console)
  console.print("#{average(1, 2)}")
end

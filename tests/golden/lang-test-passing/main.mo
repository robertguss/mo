module Main

enum Shape
  Dot
  Square(Int)
end

fn area(shape: Shape) : Int
  examples
    area(Dot) == 0
    area(Square(3)) == 9
  end

  case shape
    Dot: 0
    Square(side): side * side
  end
end

fn clamp(n: Int, low: Int, high: Int) : Int
  examples
    clamp(5, 0, 3) == 3
    clamp(-1, 0, 3) == 0
  end

  if n < low: low else if n > high: high else: n
end

test "areas add up"
  assert area(Square(2)) + area(Dot) == 4
end

test "clamp keeps values inside"
  assert clamp(2, 0, 3) == 2
end

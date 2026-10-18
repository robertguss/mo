module Main

enum Shape
  Dot
  Square(Int)
  Rectangle(width: Int, height: Int)
end

fn area(shape: Shape) : Int
  case shape
    Dot: 0
    Square(side): side * side
    Rectangle(width: w, height: h): w * h
  end
end

fn describe(shape: Shape) : Text
  case shape
    Dot: "dot"
    Square(_): "square"
    Rectangle(width: _, height: _): "rectangle"
  end
end

fn main(console: Console)
  shapes = [Dot, Square(3), Rectangle(width: 2, height: 5)]
  for s in shapes
    console.print("#{describe(s)} #{area(s)}")
  end
  console.print("#{shapes}")
end

module Geometry.Shapes
expose Shape(..), area, sides

enum Shape
  Circle(radius: Int)
  Square(side: Int)
end

fn area(shape: Shape) : Int
  case shape
    Circle(radius: r): 3 * r * r
    Square(side: s): s * s
  end
end

fn sides(shape: Shape) : Int
  case shape
    Circle(radius: _): 0
    Square(side: _): 4
  end
end

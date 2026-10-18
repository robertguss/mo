module Main
use Geometry.Shapes{Shape, area}

fn main(console: Console)
  shapes = [Square(side: 3), Circle(radius: 2)]
  for shape in shapes
    console.print("#{area(shape)}")
  end
  console.print("#{Shapes.sides(Square(side: 1))}")
end

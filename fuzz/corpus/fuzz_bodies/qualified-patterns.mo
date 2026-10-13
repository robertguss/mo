module Main
use Shapes{Shape, Box}

fn area(shape: Shape) : Decimal
  case shape
    Shapes.Circle(2.5): 1
    Shapes.Circle(radius): radius * radius
    Shapes.Dot: 0
    Shapes.Rectangle(width: w, height: h): w * h
  end
end

fn unpack(box: Box) : Int
  if box is Shapes.Box(pair: (value, _), shapes: [Shapes.Dot, .._]): value else: 0
end

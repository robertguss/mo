module Main

enum Shape
  Circle(radius: Int)
  Square(side: Int)
end

fn main(console: Console)
  console.print(Shape.bogus(Circle(radius: 1)))
end

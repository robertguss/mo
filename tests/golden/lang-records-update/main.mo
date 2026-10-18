module Main

struct Point
  x: Int
  y: Int
end

struct Box
  corner: Point
  label: Text
end

fn main(console: Console)
  original = Box(corner: Point(x: 1, y: 2), label: "a")
  var moved = original
  moved.corner.x += 10
  moved.corner.y = 7
  moved.label = "b"
  console.print("#{original.corner.x} #{original.corner.y} #{original.label}")
  console.print("#{moved.corner.x} #{moved.corner.y} #{moved.label}")
  console.print("#{moved}")
end

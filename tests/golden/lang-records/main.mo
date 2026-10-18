module Main

struct Point
  x: Int
  y: Int
end

struct Segment
  from: Point
  to: Point
end

fn length_squared(s: Segment) : Int
  dx = s.to.x - s.from.x
  dy = s.to.y - s.from.y
  dx * dx + dy * dy
end

fn main(console: Console)
  a = Point(y: 4, x: 1)
  b = Point(x: 4, y: 8)
  seg = Segment(from: a, to: b)
  console.print("#{a.x},#{a.y}")
  console.print("#{length_squared(seg)}")
  console.print("#{seg.to.y}")
  console.print("#{a == Point(x: 1, y: 4)}")
  console.print("#{a == b}")
end

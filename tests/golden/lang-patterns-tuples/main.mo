module Main

fn quadrant(p: (Int, Int)) : Text
  case p
    (0, 0): "origin"
    (x, 0) if x > 0: "positive x axis"
    (_, 0): "negative x axis"
    (0, _): "y axis"
    (x, y) if x > 0 and y > 0: "first"
    _: "elsewhere"
  end
end

fn main(console: Console)
  for p in [(0, 0), (3, 0), (-3, 0), (0, 9), (2, 2), (-1, -1)]
    console.print("#{p}: #{quadrant(p)}")
  end
end

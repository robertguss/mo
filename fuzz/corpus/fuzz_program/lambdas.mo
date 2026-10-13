module Main
fn apply(f: fn(Int) : Int, value: Int) : Int
  f(value)
end
fn main() : Int
  f = fn(value) value * 2 end
  pair = (apply(f, 4), apply(fn(value) value + 1 end, 9))
  (left, right) = pair
  left + right
end

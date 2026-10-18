module Main

fn bounds(items: List(Int)) : (Int, Int)
  (items.get(0) or 0, items.max or 0)
end

fn main(console: Console)
  pair = (3, "three")
  console.print("#{pair.0} #{pair.1}")
  (low, high) = bounds([4, 9, 2, 7])
  console.print("#{low}..#{high} spread #{high - low}")
  console.print("#{(1, 2) == (1, 2)} #{(1, 2) == (2, 1)}")
  nested = ((1, 2), "x")
  (inner, label) = nested
  console.print("#{inner.1}#{label}")
end

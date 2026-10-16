module Main
fn twice(value: T) : T where T: Number
  examples
    twice(3) == 6
    twice(1.25) == 2.5
  end
  value + value
end
fn main() : Int
  twice(9)
end

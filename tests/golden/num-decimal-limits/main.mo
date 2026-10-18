module Main

fn main(console: Console)
  console.print("#{0.000000000000000001}")
  console.print("#{0.000000000000000001 * 5}")
  console.print("#{1.5 * 0.000000000000000001}")
  console.print("#{100000000000000000000.0}")
  console.print("#{1.0 / 8}")
  console.print("#{2 / 3.0}")
  console.print("#{(-1.0) / 3}")
  console.print("#{0.5 + 0.25 + 0.125}")
end

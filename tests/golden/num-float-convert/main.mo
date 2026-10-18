module Main

fn main(console: Console)
  d = 1.5
  console.print("#{d.to_float}")
  console.print("#{3.to_decimal.to_float}")
  x = 0.1.to_float
  console.print("#{x + x + x}")
  console.print("#{x + x + x == 0.3.to_float}")
  console.print("#{x + x == 0.2.to_float}")
  console.print("#{0.25.to_float * 4.0}")
end

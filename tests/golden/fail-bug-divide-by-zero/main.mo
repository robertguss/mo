module Main

fn ratio(a: Int, b: Int) : Int
  a / b
end

fn main(console: Console)
  console.print("#{ratio(10, 3)}")
  console.print("#{10 % 4}")
  console.print("#{ratio(1, 0)}")
  console.print("unreachable")
end

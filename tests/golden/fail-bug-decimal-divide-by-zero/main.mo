module Main

fn share(total: Decimal, parts: Decimal) : Decimal
  total / parts
end

fn main(console: Console)
  console.print("#{share(1.00, 3)}")
  console.print("#{-7.5 % 2}")
  console.print("#{share(1.5, 0)}")
  console.print("unreachable")
end

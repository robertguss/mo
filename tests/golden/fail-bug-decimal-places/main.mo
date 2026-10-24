module Main

fn main(console: Console)
  price = 2.5
  console.print("#{price.round(0)} #{price.round(18)} #{price.to_fixed(0)}")
  console.print(price.to_fixed(19))
  console.print("unreachable")
end

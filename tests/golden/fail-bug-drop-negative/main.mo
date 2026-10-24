module Main

fn main(console: Console)
  items = [1, 2, 3]
  console.print("#{items.drop(0)} #{items.drop(2)} #{items.drop(5)}")
  console.print("#{items.drop(-1)}")
  console.print("unreachable")
end

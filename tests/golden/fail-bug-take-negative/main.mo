module Main

fn main(console: Console)
  items = [1, 2, 3]
  console.print("#{items.take(2)}")
  console.print("#{items.take(-1)}")
  console.print("unreachable")
end

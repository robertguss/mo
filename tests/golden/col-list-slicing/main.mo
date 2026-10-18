module Main

fn main(console: Console)
  items = [1, 2, 3, 4, 5]
  console.print("#{items.take(2)} #{items.drop(2)}")
  console.print("#{items.take(0)} #{items.drop(0)}")
  console.print("#{items.take(9)} #{items.drop(9)}")
  console.print("#{items.set(1, 20) or []}")
  console.print("#{items.remove_at(0) or []}")
  console.print("#{items.remove_at(5) or []}")
  console.print("#{items.contains?(4)} #{items.contains?(7)}")
  console.print("#{items}")
end

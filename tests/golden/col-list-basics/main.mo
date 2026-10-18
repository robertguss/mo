module Main

fn none() : List(Int)
  []
end

fn main(console: Console)
  items = [10, 20, 30]
  console.print("#{items.size} #{items.empty?} #{none().empty?}")
  console.print("#{items.first or -1} #{items.last or -1}")
  console.print("#{items.get(1) or -1} #{items.get(3) or -1} #{items.get(-1) or -1}")
  console.print("#{items.append(40)}")
  console.print("#{items.prepend(0)}")
  console.print("#{items.concat([1, 2])}")
  console.print("#{items.reverse}")
  console.print("#{items}")
end

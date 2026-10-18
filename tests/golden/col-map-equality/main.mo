module Main

fn main(console: Console)
  first = {"x": 1, "y": 2}
  second = {"y": 2, "x": 1}
  console.print("#{first == second}")
  console.print("#{first == second.remove("x")}")
  console.print("#{first.entries}")
  console.print("#{second.entries}")
  console.print("#{first.entries == second.entries}")
end

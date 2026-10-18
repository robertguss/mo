module Main

fn main(console: Console)
  console.print("before")
  console.print("#{[1, 2, 3].take(0 - 1)}")
  console.print("after")
end

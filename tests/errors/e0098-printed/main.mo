module Main

fn main(console: Console)
  console.print("hi")
  lines = console.printed
  console.print("#{lines.size}")
end

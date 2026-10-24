module Main

fn main(console: Console)
  text = "a,b"
  console.print("#{text.split(",")} #{"".split(",")}")
  console.print("#{text.split("")}")
  console.print("unreachable")
end

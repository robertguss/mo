module Main

fn main(console: Console)
  text = "ab"
  console.print("[#{text.repeat(0)}] [#{text.repeat(2)}]")
  console.print(text.repeat(-1))
  console.print("unreachable")
end

module Main

fn main(console: Console)
  text = "a-b"
  console.print(text.replace("-", ""))
  console.print(text.replace("", "+"))
  console.print("unreachable")
end

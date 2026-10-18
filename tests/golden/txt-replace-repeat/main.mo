module Main

fn main(console: Console)
  console.print("foo boo".replace("o", "0"))
  console.print("aaa".replace("aa", "b"))
  console.print("a-b-c".replace("-", ""))
  console.print("none".replace("zz", "y"))
  console.print("abc".repeat(3))
  console.print("[#{"x".repeat(0)}] [#{"=".repeat(5)}]")
  console.print("path/to/file".replace("/", " > "))
end

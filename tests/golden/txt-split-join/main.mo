module Main

fn main(console: Console)
  parts = "a,b,,c".split(",")
  console.print("#{parts} #{parts.size}")
  console.print(parts.join("|"))
  console.print("#{"".split(",")}")
  console.print("#{"one--two--three".split("--")}")
  console.print("#{"abc".split("abc")}")
  console.print(["x", "y", "z"].join(", "))
  console.print("#{"x".split(",")}")
  console.print("#{",a,".split(",")}")
end

module Main

fn main(console: Console)
  base = 2
  console.print("#{base.pow(0)} #{base.pow(10)}")
  console.print("#{base.pow(-1)}")
  console.print("unreachable")
end

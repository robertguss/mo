module Main

fn key(n: Int) : Int
  n % 3
end

fn main(console: Console)
  console.print("start")
  m = {key(1): "a", key(4): "b"}
  console.print("#{m.size}")
end

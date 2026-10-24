module Main

fn twice(n: Int) : Int
  n * 2
end

fn main(console: Console)
  console.print("#{3.twice()}")
end

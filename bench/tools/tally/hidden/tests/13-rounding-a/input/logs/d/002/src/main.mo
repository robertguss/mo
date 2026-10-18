module Main

fn f(x: Int) : Int
  x + 1
end

fn g(x: Int) : Text
  "#{x}"
end

fn main(console: Console)
  console.print(f(1))
end

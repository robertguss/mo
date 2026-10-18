module Main

fn first(items: List(T)) : Option(T)
  items.first
end

fn f(x: Int) : Int
  x + 1
end

fn g(x: Int) : Text
  "#{x}"
end

fn main(console: Console)
  console.print("#{f(1)} #{g(2)}")
end

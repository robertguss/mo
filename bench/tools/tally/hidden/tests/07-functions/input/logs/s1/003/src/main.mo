module Main

struct S
  a: Text
end

fn first(items: List(T)) : Option(T)
  items.first
end

fn f(x: Int) : Int
  y + 1
end

test "f adds one"
  assert f(1) == 2
end

fn g(x: Int) : Text
  "#{x}"
end

fn main(console: Console)
  console.print("#{f(1)} #{g(2)}")
end

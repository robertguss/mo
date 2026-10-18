module Main

struct Pair
  a: Int
  b: Int
end

fn noisy(console: Console, label: Text, n: Int) : Int
  console.print(label)
  n
end

fn main(console: Console)
  p = Pair(b: noisy(console, "b first", 2), a: noisy(console, "then a", 1))
  console.print("#{p.a} #{p.b}")
end

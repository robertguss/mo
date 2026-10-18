module Main

fn largest(items: List(T)) : Option(T) where T: Order
  items.max
end

fn no_ints() : List(Int)
  []
end

fn twice(x: T, f: fn(T) : T) : T
  f(f(x))
end

fn pair_up(a: A, b: B) : (A, B)
  (a, b)
end

fn main(console: Console)
  console.print("#{largest([3, 9, 4])}")
  console.print("#{largest(["pear", "apple", "zebra"])}")
  console.print("#{largest(no_ints())}")
  console.print("#{twice(5, fn(n) n * 3 end)}")
  console.print("#{twice("ab", fn(t) "#{t}!" end)}")
  console.print("#{pair_up(1, "one")}")
end

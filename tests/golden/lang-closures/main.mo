module Main

fn adder(n: Int) : fn(Int) : Int
  fn(x) x + n end
end

fn compose(f: fn(Int) : Int, g: fn(Int) : Int) : fn(Int) : Int
  fn(x) g(f(x)) end
end

fn double(x: Int) : Int
  x * 2
end

fn main(console: Console)
  add5 = adder(5)
  console.print("#{add5(1)}")
  both = compose(add5, double)
  console.print("#{both(1)}")
  console.print("#{[1, 2, 3].map(double)}")
  factor = 10
  console.print("#{[1, 2, 3].map(fn(x) x * factor end)}")
  var count = 0
  current = count
  show = fn() "#{current}" end
  count += 1
  console.print("#{show()} #{count}")
end

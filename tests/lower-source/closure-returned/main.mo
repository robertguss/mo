module Main

struct Counter
  name: Text
  step: fn(Int) : Int
end

fn adder(amount: Int) : fn(Int) : Int
  fn(x) x + amount end
end

fn compose(f: fn(Int) : Int, g: fn(Int) : Int) : fn(Int) : Int
  fn(x) g(f(x)) end
end

fn curried(a: Int) : fn(Int) : fn(Int) : Int
  fn(b)
    fn(c) a * 100 + b * 10 + c end
  end
end

fn counter(name: Text, by: Int) : Counter
  Counter(name: name, step: adder(by))
end

fn main(console: Console) : Int
  add_two = adder(2)
  add_five = adder(5)
  both = compose(add_two, add_five)
  digits = curried(1)(2)(3)
  steps = [adder(1), adder(10), compose(adder(1), adder(100))].map(fn(f) f(1) end)
  tick = counter("tick", 7)
  console.print(
    "#{add_two(1)}/#{add_five(1)}/#{both(1)}/#{digits}/#{steps}/#{tick.name}=#{(tick.step)(1)}"
  )
  if add_two(1) == 3 and
    both(1) == 8 and
    digits == 123 and
    steps == [2, 11, 102] and
    (tick.step)(1) == 8: 0 else: 1
end

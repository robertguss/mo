module Main

fn sum_to(n: Int, total: Int) : Int
  if n == 0: total else: sum_to(n - 1, total + n)
end

fn is_even(n: Int) : Bool
  if n == 0: true else: is_odd(n - 1)
end

fn is_odd(n: Int) : Bool
  if n == 0: false else: is_even(n - 1)
end

fn fib(n: Int) : Int
  if n < 2: n else: fib(n - 1) + fib(n - 2)
end

fn main(console: Console)
  console.print("#{sum_to(1000000, 0)}")
  console.print("#{is_even(100001)}")
  console.print("#{fib(20)}")
end

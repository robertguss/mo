module Main

fn f(a: Float, b: Float) : Float
  a / b
end

fn main(console: Console)
  zero = f(0.0, 1.0)
  console.print("#{f(1.0, 3.0)} #{f(1.0, 8.0)} #{f(1000000.0, 1.0)}")
  console.print("#{f(1.0, zero)} #{f(-1.0, zero)} #{f(zero, zero)}")
  console.print("#{f(zero, zero) == f(zero, zero)} #{f(1.0, 2.0) == f(2.0, 4.0)}")
  console.print("#{f(2.0, 3.0) < f(3.0, 4.0)} #{f(0.0, -1.0)}")
  console.print("#{f(1.0, 10000000.0)} #{f(3.0, 2.0) * 2.0 - 1.0}")
  console.print("#{f(1.0, 1.0) + 0.25} #{f(7.0, 2.0) % 2.0}")
end

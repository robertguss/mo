module Main

fn float(text: Text) : Float
  text.to_float or stop "a float literal"
end

fn main(console: Console)
  third = 1.00 / 3
  console.print("#{third.round(0)} #{third.round(18)} #{third.to_fixed(18)}")
  console.print("#{2.5.round(0)} #{(-2.5).round(0)} #{0.125.to_fixed(2)}")
  console.print("#{9223372036854775807.0.to_int} #{(-9223372036854775808.0).to_int}")
  console.print("#{9223372036854775808.0.to_int} #{(-9223372036854775809.0).to_int}")
  console.print("#{1.5.to_int} #{(-0.000000000000000001).to_int}")
  console.print("#{float("0.1").to_decimal} #{float("1.7e20").to_decimal} #{float("1.8e20").to_decimal}")
  console.print("#{float("-1.7e20").to_decimal} #{float("-1.8e20").to_decimal}")
  console.print("#{float("5e-19").to_decimal} #{float("1.5e-18").to_decimal}")
  console.print("#{float("2.5e-18").to_decimal} #{float("-3.5e-18").to_decimal}")
  console.print("#{float("6e-19").to_decimal} #{float("1e-19").to_decimal}")
  console.print("#{float("nan").to_decimal} #{float("inf").to_decimal} #{float("-inf").to_decimal}")
end

module Main

fn main(console: Console)
  power = BigInt.pow
  large = power(2.to_big, 64)
  minimum = BigInt.min
  maximum = BigInt.max
  absolute = BigInt.abs
  convert = BigInt.to_int
  console.print("#{large} #{(-large).abs} #{absolute(-large)}")
  console.print("#{minimum(large, -large)} #{maximum(-large, large)}")
  console.print("#{large.min(large)} #{large.max(large)} #{(-2.to_big).pow(3)}")
  console.print("#{(-7.to_big).floor_div(3.to_big)} #{(-7.to_big).floor_mod(3.to_big)}")
  console.print("#{BigInt.floor_div(7.to_big, -3.to_big)} #{BigInt.floor_mod(7.to_big, -3.to_big)}")
  console.print("#{0.to_big.to_int} #{large.to_int} #{convert(-large)}")
  high = 9223372036854775807.to_big
  low = (-9223372036854775808).to_big
  console.print("#{high.to_int} #{(high + 1).to_int} #{low.to_int} #{(low - 1).to_int}")
end

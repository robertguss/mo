module Main

power = BigInt.pow
huge = power(2.to_big, 100)

fn main(console: Console) : Int
  minimum = BigInt.min
  maximum = BigInt.max
  absolute = BigInt.abs
  floor_div = BigInt.floor_div
  floor_mod = BigInt.floor_mod
  to_int = BigInt.to_int
  console.print("#{(-huge).abs}/#{minimum(huge, -huge)}/#{maximum(-huge, huge)}")
  console.print(
    "#{(-7.to_big).floor_div(3.to_big)}/#{floor_div(7.to_big, -3.to_big)}/#{(-7.to_big).floor_mod(
      3.to_big
    )}/#{floor_mod(7.to_big, -3.to_big)}"
  )
  console.print(
    "#{huge.to_int}/#{to_int(-huge)}/#{9223372036854775807.to_big.to_int}/#{(-9223372036854775808)
      .to_big.to_int}"
  )
  if absolute(-huge) == huge and
    huge.min(-huge) == -huge and
    huge.max(-huge) == huge and
    minimum(-huge, huge) == -huge and
    maximum(huge, -huge) == huge and
    huge.min(huge) == huge and
    huge.max(huge) == huge and
    (-2.to_big).pow(3) == -8.to_big and
    power(0.to_big, 0) == 1.to_big and
    floor_div(-7.to_big, -3.to_big) == 2.to_big and
    floor_mod(-7.to_big, -3.to_big) == -1.to_big and
    to_int(0.to_big) == Some(0) and
    to_int(0x8000000000000000) is None and
    to_int(-0x8000000000000001) is None: 0 else: 1
end

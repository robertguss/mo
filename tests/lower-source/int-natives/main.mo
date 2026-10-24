module Main

power = Int.pow
raised = power(3, 4)

fn main(console: Console) : Int
  low = -9223372036854775808
  high = 9223372036854775807
  floor_div = Int.floor_div
  floor_mod = Int.floor_mod
  wrap_add = Int.wrapping_add
  wrap_sub = Int.wrapping_sub
  wrap_mul = Int.wrapping_mul
  sat_add = Int.saturating_add
  sat_sub = Int.saturating_sub
  sat_mul = Int.saturating_mul
  checked_sub = Int.checked_sub
  checked_mul = Int.checked_mul
  to_float = Int.to_float
  to_big = Int.to_big
  to_byte = Int.to_byte
  console.print("#{raised}/#{(-2).pow(3)}/#{0.pow(0)}/#{2.pow(0)}")
  console.print("#{(-7).floor_div(3)}/#{floor_div(7, -3)}/#{(-7).floor_mod(3)}/#{floor_mod(7, -3)}")
  console.print("#{high.wrapping_add(1)}/#{wrap_add(low, -1)}/#{low.wrapping_sub(1)}/#{wrap_sub(high, -1)}/#{high.wrapping_mul(2)}/#{wrap_mul(low, -1)}")
  console.print("#{high.saturating_add(1)}/#{sat_add(low, -1)}/#{low.saturating_sub(1)}/#{sat_sub(high, -1)}/#{high.saturating_mul(2)}/#{sat_mul(low, -1)}")
  console.print("#{low.checked_sub(1)}/#{checked_sub(7, 3)}/#{high.checked_mul(2)}/#{checked_mul(-7, 3)}")
  console.print("#{255.to_byte}/#{to_byte(256)}/#{(-1).to_byte}/#{to_byte(0)}")
  if Int.pow(2, 10) == 1024 and
    floor_div(-7, -3) == 2 and floor_mod(-7, -3) == -1 and
    low.floor_mod(-1) == 0 and
    7.wrapping_add(3) == 10 and wrap_sub(7, 3) == 4 and wrap_mul(-7, 3) == -21 and
    sat_add(7, 3) == 10 and 7.saturating_sub(3) == 4 and sat_mul(-7, 3) == -21 and
    low.saturating_mul(2) == low and high.checked_sub(-1) is None and
    checked_mul(low, -1) is None and checked_sub(low, 0) == Some(low) and
    checked_mul(high, 1) == Some(high) and
    to_float(9007199254740993) == 9007199254740992.0 and
    (-9007199254740993).to_float == -9007199254740992.0 and
    high.to_big == 0x7fffffffffffffff and to_big(low) == -0x8000000000000000 and
    (high.to_big + to_big(1)).to_text == "9223372036854775808": 0 else: 1
end

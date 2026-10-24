module Main

fn numbers(n: Float) : Text
  "#{n.abs} #{n.floor} #{n.ceil} #{n.round} #{9.0.to_float.sqrt}"
end

fn main(console: Console)
  high = 9223372036854775807
  low = -9223372036854775808
  console.print("#{(-7).floor_div(3)} #{(-7).floor_mod(3)} #{7.floor_div(-3)} #{7.floor_mod(-3)}")
  console.print("#{(-2).pow(3)} #{0.pow(0)}")
  console.print("#{high.wrapping_add(1)} #{low.wrapping_sub(1)} #{high.wrapping_mul(2)}")
  console.print("#{high.saturating_add(1)} #{low.saturating_sub(1)} #{low.saturating_mul(-1)}")
  console.print("#{low.checked_sub(1)} #{7.checked_sub(3)} #{high.checked_mul(2)} #{(-7).checked_mul(3)}")
  console.print("#{255.to_byte} #{256.to_byte} #{(-1).to_byte} #{(65.to_byte or stop "fits").to_int}")
  console.print("#{high.to_big + 1}")
  console.print("#{3.to_float} #{[2, 1, 2].to_set}")
  console.print(numbers(-2.5))
  nan = 0.0.to_float / 0.0
  infinity = 1.0.to_float / 0.0
  console.print("#{nan.nan?} #{nan.infinite?} #{infinity.infinite?} #{infinity.nan?}")
  console.print("#{0.1.to_float.to_decimal} #{nan.to_decimal} #{infinity.to_decimal}")
  bytes = "Aλ".bytes
  console.print("#{bytes.size} #{bytes.get(0)} #{bytes.get(1)} #{bytes.get(2)}")
  console.print("#{bytes.get(-1)} #{bytes.get(3)} #{bytes.decode_utf8}")
  items = [3, 1, 4, 2]
  console.print("#{items.filter(fn(n) n % 2 == 0 end)}")
  console.print("#{items.filter_map(fn(n) if n % 2 == 0: Some(n * 10) else: None end)}")
  console.print("#{items.flat_map(fn(n) [n, -n] end)}")
  console.print("#{items.any?(fn(n) n > 3 end)} #{items.any?(fn(n) n > 4 end)}")
  console.print("#{items.reduce(10, fn(a, n) a - n end)} #{items.sum}")
  groups = items.group_by(fn(n) n % 2 end)
  console.print("#{groups.keys} #{groups.get(1)} #{groups.get(0)}")
  mapped = [("b", 1), ("a", 2), ("b", 3)].to_map
  console.print("#{mapped.keys} #{mapped.values}")
end

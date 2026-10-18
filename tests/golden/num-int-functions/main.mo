module Main

fn main(console: Console)
  console.print("#{(-5).abs} #{5.abs} #{0.abs}")
  console.print("#{3.min(9)} #{3.max(9)} #{(-3).min(-9)} #{(-3).max(-9)}")
  console.print("#{3 < 4} #{3 >= 4} #{3 != 3} #{3 == 3}")
  console.print("#{65.to_char} #{97.to_char}")
  console.print("#{1_114_112.to_char}")
  console.print("#{55296.to_char}")
end

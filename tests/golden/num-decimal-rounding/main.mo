module Main

fn main(console: Console)
  x = 2.675
  console.print("#{x.round(2)} #{x.round(0)} #{x.round(1)}")
  console.print("#{(-2.5).round(0)} #{2.5.round(0)} #{3.5.round(0)}")
  console.print("#{x.floor} #{x.ceil} #{(-x).floor} #{(-x).ceil}")
  console.print(x.to_fixed(2))
  console.print(x.to_fixed(0))
  console.print(3.to_decimal.to_fixed(3))
  console.print("#{0.5.to_fixed(0)} #{1.5.to_fixed(0)}")
  console.print("#{(12.34).abs} #{(-12.34).abs}")
  console.print("#{1.5.min(2.5)} #{1.5.max(2.5)}")
end

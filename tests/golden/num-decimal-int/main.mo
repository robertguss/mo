module Main

fn main(console: Console)
  console.print("#{4.0.to_int}")
  console.print("#{4.5.to_int}")
  console.print("#{(-9.0).to_int}")
  cents = 1999
  share = cents.to_decimal / 100
  console.print("#{share}")
  console.print(share.to_fixed(2))
  console.print("#{share * 3}")
  console.print("#{7.to_decimal / 2.to_decimal}")
end

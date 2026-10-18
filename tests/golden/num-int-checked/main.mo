module Main

fn main(console: Console)
  big = 9223372036854775807
  console.print("#{big.checked_add(1)}")
  console.print("#{big.checked_add(0)}")
  console.print("#{(-big).checked_add(-1)}")
  console.print("#{(-big).checked_add(-2)}")
  console.print("#{3.checked_add(4)}")
end

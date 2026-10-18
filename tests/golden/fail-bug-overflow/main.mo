module Main

fn main(console: Console)
  big = 9223372036854775807
  console.print("#{big}")
  console.print("#{big - 1 + 1}")
  console.print("#{big + 1}")
  console.print("unreachable")
end

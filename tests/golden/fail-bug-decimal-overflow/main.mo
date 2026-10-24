module Main

fn main(console: Console)
  big = 100000000000000000000.0
  console.print("#{big}")
  console.print("#{big + big / 2}")
  console.print("#{big * 2}")
  console.print("unreachable")
end

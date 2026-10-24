module Main

fn main(console: Console)
  long = 9223372036854.seconds
  console.print("#{long}")
  console.print("#{long - 1.seconds + 1.seconds}")
  console.print("#{long + 1.seconds}")
  console.print("unreachable")
end

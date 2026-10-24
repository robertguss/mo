module Main

fn main(console: Console)
  long = 9223372036854.seconds
  console.print("#{long}")
  console.print("#{long - long}")
  console.print("#{long + long}")
  console.print("unreachable")
end

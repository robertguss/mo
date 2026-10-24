module Main

fn main(console: Console)
  console.print("#{9223372036854.seconds}")
  console.print("#{9223372036855.seconds}")
  console.print("unreachable")
end

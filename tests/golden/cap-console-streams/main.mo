module Main

fn warn(console: Console, text: Text)
  console.error("warning: #{text}")
end

fn main(console: Console)
  console.print("one")
  warn(console, "two")
  console.print("three")
  console.error("four")
end

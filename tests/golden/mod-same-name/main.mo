module Main
use Left
use Right

fn main(console: Console)
  console.print("#{Left.value()} #{Right.value()}")
end

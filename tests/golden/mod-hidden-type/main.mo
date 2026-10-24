module Main
use Bank.Counter

fn main(console: Console)
  c = Counter.start().bump.bump.bump
  console.print("#{Counter.value(c)}")
  console.print("#{Counter.value(Counter.start())}")
end

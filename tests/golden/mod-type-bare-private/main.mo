module Main
use Secret

fn show(_s: Secret) : Int
  1
end

fn main(console: Console)
  console.print("#{show(Secret.make())}")
end

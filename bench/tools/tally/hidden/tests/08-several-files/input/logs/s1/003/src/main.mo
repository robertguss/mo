module Main

use Util

fn helper(x: Int) : Int
  x + 2
end

fn main(console: Console)
  console.print("#{helper(1)} #{Util.helper(2)}")
end

module Main

fn duplicate(first: Text, second: Text) : Int
  entries = {first: 1, second: 2}
  entries.size
end

fn main() : Int
  duplicate("apples", "apples")
end

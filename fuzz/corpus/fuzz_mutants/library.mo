module Main

fn total(items: List(Int)) : Int
  items.count(fn(n) n % 2 == 0 end) + "#{items.size <= 3}".byte_count
end

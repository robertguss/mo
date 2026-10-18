module Main

fn main(console: Console)
  items = [1, 2, 3, 4, 5, 6]
  console.print("#{items.map(fn(n) n * n end)}")
  console.print("#{items.find(fn(n) n > 3 end) or 0}")
  console.print("#{items.find(fn(n) n > 30 end) or 0}")
  console.print("#{items.all?(fn(n) n > 1 end)} #{items.all?(fn(n) n > 0 end)}")
  console.print("#{items.count(fn(n) n % 2 == 1 end)}")
  console.print("#{items.max or 0}")
  console.print("#{items.sort_by_desc(fn(n) n % 3 end)}")
  console.print("#{[3, 1, 3, 2, 1].unique}")
end

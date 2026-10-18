module Main

fn main(console: Console)
  stock = {"apples": 3, "pears": 5}
  console.print("#{stock.size} #{stock.empty?}")
  console.print("#{stock.get("apples") or 0} #{stock.get("plums") or 0}")
  console.print("#{stock.has?("pears")} #{stock.has?("plums")}")
  console.print("#{stock.keys}")
  console.print("#{stock.values}")
  for (fruit, n) in stock
    console.print("#{fruit}: #{n}")
  end
end

module Main
fn run(items: List(Int)) : Int
  total = 0
  for item in items
    total = total + item
  end
  total
end

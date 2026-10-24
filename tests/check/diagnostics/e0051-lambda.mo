module Main
fn run(item: Int) : Int
  total = 0
  store = fn(x)
    total = x
    total
  end
  store(item)
end

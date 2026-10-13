module Main
fn run(pair: ((Text, Int), Bool), f: fn() : fn() : Int) : Int
  assert pair.0.0 == "text"
  if pair.1: pair.0.1 else: f()()
end

module Main
fn run(flag: Bool) : Int
  var total = 0
  for i in 0..3
    continue if i % 2 == 0
    total += i
  end
  if flag: total else: 0
end

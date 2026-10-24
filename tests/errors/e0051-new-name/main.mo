module Main
fn run(items: List(Int)) : Text
  result = 0
  for item in items
    result = "#{item}"
  end
  "#{result}"
end

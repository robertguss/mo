module Main

fn main(console: Console) : Int
  var number = 3
  current = number
  entries = {"first": fn() current end, "second": fn() current + 1 end}
  number = 19
  first = (entries.get("first") or fn() 0 end)()
  second = (entries.get("second") or fn() 0 end)()
  console.print("#{first}/#{second}/#{number}")
  if first == 3 and second == 4 and number == 19: 0 else: 1
end

module Main

fn main(console: Console) : Int
  var key = 1
  entries = {key: case true
    true:
      key = 2
      3
    false: 4
    end, key: 5}
  var ordered = ""
  for (entry_key, value) in entries
    ordered = "#{ordered}#{entry_key}=#{value};"
  end
  console.print(ordered)
  if entries.get(1) == Some(3) and entries.get(2) == Some(5) and ordered == "1=3;2=5;": 0 else: 1
end

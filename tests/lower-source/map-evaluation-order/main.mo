module Main

fn emit_key(console: Console, value: Int) : Int
  console.print("key #{value}")
  value
end

fn emit_value(console: Console, value: Int) : Int
  console.print("value #{value}")
  value
end

fn main(console: Console) : Int
  entries = {emit_key(console, 2): emit_value(console, 20), emit_key(console, 1): emit_value(console, 10)}
  for (key, value) in entries
    console.print("#{key}=#{value}")
  end
  if true
    console.print("if effect")
  end
  var round = 0
  while round < 1
    round += 1
    console.print("while effect")
  end
  if entries.get(2) == Some(20) and entries.get(1) == Some(10): 0 else: 1
end

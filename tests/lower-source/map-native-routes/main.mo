module Main

fn through_values(get: fn(Map(Text, Int), Text) : Option(Int), set: fn(Map(Text, Int), Text, Int) : Map(Text, Int), size: fn(Map(Text, Int)) : Int) : Int
  entries = set({"value": 7}, "value", 11)
  (get(entries, "value") or 0) + size(entries)
end

fn main(console: Console) : Int
  get = Map.get
  set = Map.set
  size = Map.size
  original = {"first": 3}
  snapshot = original
  var current = Map.set(original, "second", 7)
  current = current.set("first", 11)
  direct = through_values(Map.get, Map.set, Map.size)
  aliases = through_values(get, set, size)
  console.print("#{Map.get(current, "first") or 0}/#{get(snapshot, "first") or 0}/#{size(current)}/#{direct}/#{aliases}")
  if Map.get(current, "first") == Some(11) and get(snapshot, "first") == Some(3) and size(current) == 2 and direct == 12 and aliases == 12: 0 else: 1
end

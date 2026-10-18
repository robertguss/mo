module Main

fn show(console: Console, m: Map(Text, Int))
  for (k, v) in m
    console.print("#{k}=#{v}")
  end
  console.print("--")
end

fn main(console: Console)
  m = {"a": 1, "b": 2, "c": 3}
  show(console, m.set("b", 20))
  show(console, m.remove("a"))
  show(console, m.remove("a").set("a", 9))
  show(console, m.update("c", 0, fn(v) v * 10 end))
  show(console, m.update("z", 5, fn(v) v * 10 end))
  show(console, m)
end

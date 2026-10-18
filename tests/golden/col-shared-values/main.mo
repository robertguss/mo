module Main

fn main(console: Console)
  original = [1, 2, 3]
  longer = original.append(4)
  changed = original.set(0, 99) or []
  console.print("#{original}")
  console.print("#{longer}")
  console.print("#{changed}")
  var a = {"k": 1}
  b = a
  a = a.set("k", 2)
  console.print("#{a.get("k") or 0} #{b.get("k") or 0}")
  var shared = [10, 20]
  keep = shared
  shared = shared.reverse
  console.print("#{shared} #{keep}")
end

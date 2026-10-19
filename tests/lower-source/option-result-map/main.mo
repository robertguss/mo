module Main

enum Problem
  Missing(name: Text)
  Empty
end

fn lookup(names: Map(Text, Int), name: Text) : Result(Int, Problem)
  names.get(name).or_fail(Missing(name: name))
end

fn describe(problem: Problem) : Text
  case problem
    Missing(name: name): "no #{name}"
    Empty: "empty"
  end
end

fn doubled(names: Map(Text, Int), name: Text) : Result(Int, Text)
  value = try lookup(names, name).map_error(describe)
  Ok(value * 2)
end

fn main(console: Console)
  names = {"a": 1, "b": 2}
  console.print("#{Some(2).map(fn(n) n * 10 end)}/#{names.get("z").map(fn(n) n * 10 end)}")
  console.print(
    "#{Some("abc").map(fn(t) t.byte_count end)}/#{Option.map(Some(4), fn(n) n - 1 end)}"
  )
  console.print(
    "#{lookup(names, "b").map(fn(n) n * 10 end)}/#{lookup(names, "z").map(fn(n) n * 10 end)}"
  )
  console.print(
    "#{lookup(names, "a").map_error(describe)}/#{lookup(names, "z").map_error(describe)}"
  )
  console.print("#{doubled(names, "b")}/#{doubled(names, "q")}")
  wrap = Result.map
  console.print("#{wrap(lookup(names, "a"), fn(n) n == 1 end)}")
  zero = names.size - 2
  console.print(
    "#{names.get("z").map(fn(n) n / zero end)}/#{lookup(names, "z").map(fn(n) n / zero end)}"
  )
  console.print("#{lookup(names, "a").map_error(fn(p) describe(p).byte_count / zero end)}")
end

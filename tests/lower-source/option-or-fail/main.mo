module Main

enum Problem
  Missing(name: Text)
  Empty
end

fn lookup(names: Map(Text, Int), name: Text) : Result(Int, Problem)
  names.get(name).or_fail(Missing(name: name))
end

fn first_twice(values: List(Int)) : Result(Int, Problem)
  value = try values.get(0).or_fail(Empty)
  Ok(value * 2)
end

fn noisy(console: Console, text: Text) : Problem
  console.print("made #{text}")
  Missing(name: text)
end

fn main(console: Console)
  names = {"a": 1, "b": 2}
  console.print("#{lookup(names, "a")}/#{lookup(names, "z")}")
  console.print("#{first_twice([21])}/#{first_twice([])}")
  console.print("#{Some(3).or_fail(Empty)}/#{[1].get(5).or_fail(Empty)}")
  wrap = Option.or_fail
  console.print("#{wrap(Some("x"), 7)}/#{wrap(None, 7)}")
  console.print("#{Some(1).or_fail(noisy(console, "eager"))}")
end

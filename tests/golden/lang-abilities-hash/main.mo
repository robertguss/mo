module Main

struct Key
  group: Text
  id: Int
end

fn main(console: Console)
  var counts = {Key(group: "a", id: 1): 10}
  counts = counts.set(Key(group: "b", id: 2), 20)
  counts = counts.set(Key(group: "a", id: 1), 11)
  console.print("#{counts.size}")
  console.print("#{counts.get(Key(group: "a", id: 1))}")
  console.print("#{counts.get(Key(group: "z", id: 9))}")
  console.print("#{counts.has?(Key(group: "b", id: 2))}")
  console.print("#{[1, 2, 3] == [1, 2, 3]} #{{"x": 1, "y": 2} == {"y": 2, "x": 1}}")
end

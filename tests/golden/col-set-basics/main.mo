module Main

fn main(console: Console)
  var seen = Set.empty.add(3)
  seen = seen.add(1)
  seen = seen.add(3)
  console.print("#{seen.size} #{seen.has?(1)} #{seen.has?(2)}")
  console.print("#{seen.to_list}")
  seen = seen.remove(3).add(3)
  console.print("#{seen.to_list}")
  other = Set.empty.add(1).add(7)
  console.print("#{seen.union(other).to_list}")
  console.print("#{seen.intersection(other).to_list}")
  console.print("#{seen.difference(other).to_list}")
  console.print("#{seen.empty?} #{Set.empty.add(1).remove(1).empty?}")
end

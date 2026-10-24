module ListBuilding

fn main(console: Console)
  var values = [0].take(0)
  for i in 0..100_000
    values = values.append(i)
  end
  console.print("#{values.size} #{values.sum}")
end

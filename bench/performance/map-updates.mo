module MapUpdates

fn main(console: Console)
  var counts = {0: 0}.remove(0)
  for i in 0..100_000
    counts = counts.update(i % 1_000, 0, fn(n) n + 1 end)
  end
  console.print("#{counts.size} #{counts.values.sum} #{counts.get(999) or 0}")
end

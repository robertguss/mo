module Main

fn main(console: Console)
  words = ["to", "be", "or", "not", "to", "be"]
  var counts = {"": 0}.remove("")
  for word in words
    counts = counts.update(word, 0, fn(n) n + 1 end)
  end
  for (word, n) in counts
    console.print("#{word} #{n}")
  end
  console.print("#{counts.size}")
end

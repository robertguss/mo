module Main

fn main(console: Console)
  var counts = {"": 0}.remove("")
  for word in "the cat and the hat and the bat".words
    counts = counts.update(word, 0, fn(n) n + 1 end)
  end
  for (word, n) in counts
    console.print("#{word.pad_right(4)}#{n}")
  end
  console.print("#{counts.size} distinct, #{counts.get("the") or 0} the, #{counts.get("dog") or 0} dog")
end

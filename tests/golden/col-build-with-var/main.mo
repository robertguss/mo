module Main

fn main(console: Console)
  var squares = [0]
  squares = squares.take(0)
  for i in 1..6
    squares = squares.append(i * i)
  end
  console.print("#{squares}")
  var evens = squares
  evens = evens.prepend(0)
  console.print("#{evens.size} #{squares.size}")
  var words = ["b"]
  words = words.concat(["c", "d"])
  words = words.prepend("a")
  console.print(words.join("-"))
end

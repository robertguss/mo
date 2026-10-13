module Main
struct Pair
  left: Int
  right: Int
end
fn main(console: Console) : Int
  var pair = Pair(right: 9, left: 4)
  pair.left -= 1
  for (number, include?) in [(4, true), (9, false)]
    if include?
      pair.left += number
    end
  end
  while pair.left < 10
    break if pair.left == 9
    pair.left += 1
  end
  console.print("done")
  case Some(pair.left)
    Some(n): n
    None: 0
  end
end

module Main

fn swap(pair: (A, B)) : (B, A) where A: Equal, B: Equal
  promise "the two values trade places"
    result.0 == pair.1
    result.1 == pair.0
    allows pair: (1, "a"), result: ("a", 1)
    forbids pair: (1, "a"), result: ("b", 1)
  end
  examples
    swap((1, "a")) == ("a", 1)
    swap((true, 2.5)) == (2.5, true)
  end

  (pair.1, pair.0)
end

fn larger(a: T, b: T) : T where T: Order
  examples
    larger(3, 9) == 9
    larger("b", "a") == "b"
  end

  if a < b: b else: a
end

fn main(console: Console) : Int
  swapped = swap((7, "x"))
  biggest = larger(2.5, 1.5)
  console.print("#{swapped}/#{biggest}/#{larger('a', 'z')}")
  if swapped == ("x", 7) and biggest == 2.5 and larger('a', 'z') == 'z': 0 else: 1
end

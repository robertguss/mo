module Main

fn classify(n: Int) : Text
  case n
    0: "zero"
    1 | 2 | 3: "small"
    x if x < 0: "negative"
    x if x % 2 == 0: "even"
    _: "odd"
  end
end

fn main(console: Console)
  for n in [0, 2, -5, 10, 11]
    console.print("#{n} is #{classify(n)}")
  end
end

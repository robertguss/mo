module Main

fn describe(items: List(Int)) : Text
  case items
    []: "empty"
    [a]: "one: #{a}"
    [a, b]: "two: #{a} and #{b}"
    [first, ..rest]: "starts with #{first}, then #{rest.size} more"
  end
end

fn total(items: List(Int)) : Int
  case items
    []: 0
    [first, ..rest]: first + total(rest)
  end
end

fn main(console: Console)
  for items in [[], [7], [1, 2], [4, 5, 6, 7]]
    console.print(describe(items))
  end
  console.print("#{total([1, 2, 3, 4])}")
end

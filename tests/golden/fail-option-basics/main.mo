module Main

fn first_even(items: List(Int)) : Option(Int)
  items.find(fn(n) n % 2 == 0 end)
end

fn describe(found: Option(Int)) : Text
  case found
    Some(n): "found #{n}"
    None: "nothing"
  end
end

fn main(console: Console)
  console.print("#{first_even([1, 3, 4, 6]) or -1}")
  console.print("#{first_even([1, 3]) or -1}")
  console.print(describe([5, 8].get(1)))
  console.print(describe([5, 8].get(2)))
  console.print(describe("12".to_int))
  console.print(describe("x12".to_int))
end

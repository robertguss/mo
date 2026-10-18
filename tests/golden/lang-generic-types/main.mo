module Main

struct Page(T)
  items: List(T)
  number: Int
end

enum Either(L, R)
  Left(L)
  Right(R)
end

fn first_item(page: Page(T)) : Option(T)
  page.items.first
end

fn flip(e: Either(A, B)) : Either(B, A)
  case e
    Left(a): Right(a)
    Right(b): Left(b)
  end
end

fn left_three() : Either(Int, Text)
  Left(3)
end

fn right_text() : Either(Int, Text)
  Right("r")
end

fn main(console: Console)
  numbers = Page(items: [10, 20], number: 1)
  words = Page(items: ["x"], number: 2)
  console.print("#{first_item(numbers)} #{first_item(words)}")
  console.print("#{numbers}")
  console.print("#{flip(left_three())}")
  console.print("#{flip(right_text())}")
end

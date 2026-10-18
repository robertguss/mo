module Main

struct Card
  rank: Int
  name: Text
end

fn compare(a: Card, b: Card) : Ordering
  b.rank.compare(a.rank)
end

fn main(console: Console)
  ace = Card(rank: 14, name: "ace")
  two = Card(rank: 2, name: "two")
  verdict = case ace.compare(two)
    Less: "ace first"
    Equal: "tie"
    Greater: "two first"
  end
  console.print("#{ace < two} #{verdict} #{two > ace}")
  best = [two, ace].max
  console.print("#{best}")
end

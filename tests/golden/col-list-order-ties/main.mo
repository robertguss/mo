module Main

struct Card
  rank: Int
  name: Text
end

fn compare(a: Card, b: Card) : Ordering
  a.rank.compare(b.rank)
end

fn names(cards: List(Card)) : Text
  cards.map(fn(card) card.name end).join(" ")
end

fn main(console: Console)
  cards = [
    Card(rank: 2, name: "two-a"),
    Card(rank: 9, name: "nine-a"),
    Card(rank: 2, name: "two-b"),
    Card(rank: 9, name: "nine-b"),
    Card(rank: 5, name: "five")
  ]
  console.print("#{cards.min.map(fn(card) card.name end)} #{cards.max.map(fn(card) card.name end)}")
  console.print(names(cards.sort))
  console.print(names(cards.reverse.sort))
  console.print(names(cards.sort_by(fn(card) card.rank % 2 end)))
  console.print(names(cards.sort_by_desc(fn(card) card.rank % 2 end)))
  words = ["bb", "a", "cc", "d", "ee"]
  console.print("#{words.sort_by(fn(word) word.byte_count end)}")
  console.print("#{[3, 1, 3, 1].min} #{[5].drop(1).max}")
end

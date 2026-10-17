module Main

struct Ranked
  rank: Int
  name: Text
end

fn compare(a: Ranked, b: Ranked) : Ordering
  if a.rank < b.rank: Less else if a.rank > b.rank: Greater else: Equal
end

biggest = [3, 9, 4].max
fallback = 7

fn empty_ints() : List(Int)
  []
end

fn main(console: Console) : Int
  empty = empty_ints().max or fallback
  single = [5].max or fallback
  ints = [2, 8, 5, 8, 1].max or fallback
  negatives = [-4, -9, -2].max or fallback
  words = ["pear", "apple", "zebra", "mango"].max or "?"
  first_of_equal = [Ranked(rank: 1, name: "a"), Ranked(rank: 2, name: "b"),
    Ranked(rank: 2, name: "c")].max
  tie_name = case first_of_equal
    Some(e): e.name
    None: "?"
  end
  mapped = ["a", "bcd", "ef"].map(fn(w) w.byte_count end).max or 0
  qualified = List.max([1, 6, 3]) or fallback
  largest = List.max
  via_value = largest([10, 30, 20]) or fallback
  console.print("#{empty}/#{single}/#{ints}/#{negatives}/#{words}/#{tie_name}/#{mapped}/#{qualified}/#{via_value}/#{biggest or fallback}")
  if empty == 7 and single == 5 and ints == 8 and negatives == -2 and tie_name == "b" and mapped == 3: 0 else: 1
end

module Main

fn empty_ints() : List(Int)
  []
end

fn main(console: Console) : Int
  pairs = ["a", "b", "c"].zip([1, 2])
  none = ["a"].zip(empty_ints())
  wide = [1, 2].zip(["x", "y", "z"])
  qualified = List.zip([true, false], ["t", "f"])
  counts = {"b": 2, "a": 1}.set("c", 3)
  entries = counts.entries
  console.print("#{pairs}/#{none}/#{wide}/#{qualified}/#{entries}")
  if pairs.size == 2 and none.size == 0 and wide.size == 2 and entries.size == 3: 0 else: 1
end

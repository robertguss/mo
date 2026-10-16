module Main

struct Counts
  entries: Map(Text, Int)
end

fn empty() : Map(Text, Int)
  {}
end

fn count(entries: Map(Text, Int)) : Int
  entries.size
end

fn main(console: Console) : Int
  fruit = "apples"
  entries = {fruit: 3, "pears": 7}
  boxed = Counts(entries: {})
  var later = {}
  later = later.set("plums", 11)
  console.print("#{entries.get(fruit) or 0}/#{entries.get("pears") or 0}/#{boxed.entries.size}/#{count({})}/#{empty().size}/#{later.get("plums") or 0}")
  var ordered = ""
  for (key, value) in entries
    ordered = "#{ordered}#{key}=#{value};"
  end
  console.print(ordered)
  if entries.get(fruit) == Some(3) and entries.get("pears") == Some(7) and boxed.entries.size == 0 and count({}) == 0 and empty().size == 0 and later.get("plums") == Some(11) and ordered == "apples=3;pears=7;": 0 else: 1
end

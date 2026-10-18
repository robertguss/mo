module Main

struct Score
  name: Text
  points: Int
end

fn main(console: Console)
  names = ["ada", "bo", "cy"]
  points = [30, 10, 20]
  scores = names.zip(points).map(fn(pair) Score(name: pair.0, points: pair.1) end)
  ranked = scores.sort_by_desc(fn(s) s.points end)
  for (rank, s) in ranked.enumerate
    console.print("#{rank + 1}. #{s.name} #{s.points}")
  end
  best = ranked.first
  console.print("#{best}")
end

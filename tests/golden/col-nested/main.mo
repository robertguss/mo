module Main

fn main(console: Console)
  grid = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
  var diagonal = 0
  for i in 0..3
    row = grid.get(i) or []
    diagonal += row.get(i) or 0
  end
  console.print("diagonal #{diagonal}")
  var flat = [0].drop(1)
  for row in grid
    flat = flat.concat(row)
  end
  console.print("#{flat}")
  sums = grid.map(fn(row) row.count(fn(n) n % 2 == 1 end) end)
  console.print("#{sums}")
  index = {"evens": [2, 4], "odds": [1, 3, 5]}
  for (name, members) in index
    console.print("#{name}: #{members.size}")
  end
  console.print("#{(index.get("odds") or []).last or 0}")
end

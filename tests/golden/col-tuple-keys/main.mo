module Main

fn main(console: Console)
  var grid = {(0, 0): "origin"}
  grid = grid.set((1, 2), "a")
  grid = grid.set((0, 0), "start")
  console.print(grid.get((0, 0)) or "?")
  console.print(grid.get((1, 2)) or "?")
  console.print(grid.get((2, 1)) or "?")
  console.print("#{grid.size} #{grid.keys}")
end

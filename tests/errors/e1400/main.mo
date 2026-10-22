module Main

fn twice(xs: List(Int), f: fn(Int) : Int) : List(Int)
  promise "keeps the length"
    result.size == xs.size
    allows xs: [1], f: fn(x) x end, result: [1]
    forbids xs: [1], f: fn(x) x end, result: []
  end

  xs.map(fn(x) f(f(x)) end)
end

module Main
f = fn(x) [x, x] end
empty = fn() [] end
nested = fn(x) [fn(y) [x, y] end, x] end
mapped = xs.map(fn(x) [x, x] end)

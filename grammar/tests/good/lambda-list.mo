f = fn(x) [x, x] end
empty = fn() [] end
xs.map(fn(x) [x, x] end)
multi = fn(
  x,
  y
) [x, y] end
nested = fn(x) [fn(y) [x, y] end, x] end
calls = fn(x) [g(x), (x)] end
text = fn(x) "#{[x].map(fn(y) [y] end)}" end

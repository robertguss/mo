# Statements (reference 5 and 6).
x = 1
var y = 2
y = 3
y += 1
y -= 1
var c = cart
c.items = c.items.append(item)
c.totals.net += 1
(low, high) = bounds(xs)
(_, (a, b)) = nested
for item in items
  continue if item.skip?
  break if item.last?
  print(item)
end
for _ in 0..3
  tick()
end
for (key, value) in map
  use_both(key, value)
end
for (i, (k, v)) in map.entries.enumerate
  show(i, k, v)
end
while queue is [next, ..rest]
  queue = rest
end
assert x == 1
assert reply is Ok(body)
label = if n == 1: "one" else: "many"
sign = if n < 0: "-" else if n == 0: "0" else: "+"
grade = if score > 90
  "A"
else if score > 80
  "B"
else
  "C"
end
if done?
  finish()
end
result = case outcome
  Ok(v): v
  Error(e):
    report(e)
    0
end
case command
  Add(text: t): items = items.append(t)
  Clear: items = []
  Stop: return
  Skip: continue
  Done: break
  Fail(reason: r): stop "unreachable: #{r}"
end
f = fn(a, b) a + b end
g = fn() 42 end
h = fn(_) 0 end
k = fn(x)
  y = x + 1
  y * 2
end
values = items.map(fn(item)
  item.price
end).sum

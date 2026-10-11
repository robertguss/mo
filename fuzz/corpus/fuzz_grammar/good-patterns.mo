# Every kind of pattern (reference 7.3), in case arms and after is.
case value
  _: 0
end
case value
  name: name
end
case n
  42: "answer"
  -1: "minus one"
  2.5: "two and a half"
  0 | 1: "small"
  _ if n > 100: "big"
  _: "other"
end
case text
  "GET": 1
  "": 0
  _: 2
end
case c
  'x' | 'y': true
  _: false
end
case flag
  true: 1
  false: 0
end
case option
  Some(Some(x)): x
  Some(None) | None: 0
end
case shape
  Dot: 0
  Circle(r): r
  Rectangle(width: w, height: h): w * h
  Shapes.Triangle(base: b): b
end
case pair
  (0, y): y
  (x, _): x
end
case items
  []: 0
  [only]: only
  [first, second]: first + second
  [first, ..rest]: first
end
case (name, args)
  ("SET", [key, value]): 1
  ("DEL", [_, .._]): 2
  _: 3
end
ok = reply is Ok(Page(items: [first, ..rest]))
negative = n is -1

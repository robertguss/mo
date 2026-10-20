module Main

fn apply_twice(f: fn(T) : T, value: T) : T
  f(f(value))
end

fn transform(items: List(T), f: fn(T) : U) : List(U)
  case items
    []: []
    [first, ..rest]: transform(rest, f).prepend(f(first))
  end
end

fn fold(items: List(T), start: A, f: fn(A, T) : A) : A
  var acc = start
  for item in items
    acc = f(acc, item)
  end
  acc
end

fn then(f: fn(A) : B, g: fn(B) : C) : fn(A) : C
  fn(x) g(f(x)) end
end

fn pair_with(value: T) : fn(U) : (T, U)
  fn(other) (value, other) end
end

fn main(console: Console) : Int
  ints = apply_twice(fn(n) n * 3 end, 2)
  texts = apply_twice(fn(t) "<#{t}>" end, "x")
  lengths = transform(["a", "bcd", ""], Text.byte_count)
  shown = transform([1, 2], fn(n) "#{n}!" end)
  sum = fold([1, 2, 3, 4], 0, fn(a, n) a + n end)
  joined = fold([3, 4], "", fn(a, n) "#{a}#{n}," end)
  measure = then(Text.byte_count, fn(n) n > 2 end)
  tagged = pair_with("k")(7)
  console.print(
    "#{ints}/#{texts}/#{lengths}/#{shown}/#{sum}/#{joined}/#{measure("abc")}/#{measure("ab")}/#{tagged}"
  )
  if ints == 18 and
    texts == "<<x>>" and
    lengths == [1, 3, 0] and
    shown == ["1!", "2!"] and
    sum == 10 and
    joined == "3,4," and
    measure("abc") and
    not measure("ab") and
    tagged == ("k", 7): 0 else: 1
end

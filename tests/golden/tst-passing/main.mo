module Main

fn initials(name: Text) : Text
  examples
    initials("Ada Lovelace") == "AL"
    initials("") == ""
  end

  firsts = name.words.map(fn(word) word.chars.first or '?' end)
  Text.from_chars(firsts)
end

fn ordered(a: Int, b: Int) : (Int, Int)
  promise "the smaller comes first, and both values are kept"
    result.0 <= result.1
    result == (a, b) or result == (b, a)
    allows a: 5, b: 2, result: (2, 5)
    forbids a: 5, b: 2, result: (2, 2)
  end

  if a <= b: (a, b) else: (b, a)
end

test "initials of a short name"
  assert initials("Grace Hopper") == "GH"
end

test "ordering is stable on ties"
  assert ordered(4, 4) == (4, 4)
end

fn main(console: Console)
  console.print(initials("Ada Lovelace"))
end

module Main

fn between?(a: Int, b: Int, c: Int) : Bool
  a < b < c
end

fn all_same?(a: Int, b: Int, c: Int) : Bool
  a == b == c
end

fn steps() : Range
  1..2..3
end

fn follows?(a: Bool, b: Bool, c: Bool) : Bool
  a implies b implies c
end

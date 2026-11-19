module Main

fn neither(a: Bool, b: Bool) : Bool
  not (a or b)
end

fn head(xs: List(Int)) : Int
  xs.first or 0
end

test "nothing calls neither or head"
  assert true
end

fn main()
end

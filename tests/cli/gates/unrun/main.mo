module Main

fn neither(a: Bool, b: Bool) : Bool
  not (a or b)
end

test "nothing calls neither"
  assert true
end

fn main()
end

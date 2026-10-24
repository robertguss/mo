module Main

fn value_or_zero(x: Option(Int)) : Int
  if x is Some(n)
    return n
  end
  n
end

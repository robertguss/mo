module Main

fn both(a: Bool, b: Bool) : Bool
  if a or b
    stop "no answer"
  end
  false
end

fn half(n: Int) : Int
  n / 3
end

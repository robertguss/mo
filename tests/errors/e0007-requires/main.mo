module Main

fn half(n: Int) : Int
  requires n >= 0
  n / 2
end

fn third(n: Int) : Int
  precondition n >= 0
  n / 3
end

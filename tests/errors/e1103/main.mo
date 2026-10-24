module Main

answer = invoke(fn() 13 end)

fn invoke(action: fn() : Int) : Int
  action()
end

fn main() : Int
  answer
end

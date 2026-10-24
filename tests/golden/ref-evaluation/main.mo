module Main

fn number(console: Console, value: Int) : Int
  console.print("number #{value}")
  value
end

fn combine(console: Console, left: Int, right: Int) : Int
  console.print("combine")
  left * 10 + right
end

fn truth(console: Console, value: Bool) : Bool
  console.print("truth #{value}")
  value
end

fn failure(console: Console) : Text
  console.print("eager failure argument")
  "unused"
end

fn main(console: Console)
  console.print("#{combine(console, number(console, 2), number(console, 7))}")
  console.print("#{number(console, 3) - number(console, 1)}")
  console.print("#{false and truth(console, true)}")
  console.print("#{true or truth(console, false)}")
  console.print("#{false implies truth(console, false)}")
  console.print("#{true and truth(console, false)}")
  console.print("#{false or truth(console, true)}")
  console.print("#{true implies truth(console, false)}")
  console.print("#{not true} #{not false}")
  console.print("#{Some(4) or number(console, 99)}")
  missing = "bad".to_int
  console.print("#{missing or number(console, 6)}")
  console.print("#{Some(4).or_fail(failure(console))}")
end

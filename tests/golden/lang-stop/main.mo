module Main

enum Mode
  Fast
  Slow
end

fn speed(mode: Mode) : Int
  case mode
    Fast: 10
    Slow: stop "slow mode is not supported"
  end
end

fn main(console: Console)
  console.print("#{speed(Fast)}")
  console.print("#{speed(Slow)}")
  console.print("unreachable")
end

module Main
use Shop.Red

enum Color
  Red
  Green
end

fn shade(color: Color) : Text
  case color
    Red: "red"
    Green: "green"
  end
end

fn main(console: Console)
  console.print(Red.shade())
end

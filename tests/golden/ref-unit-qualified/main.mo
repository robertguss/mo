module Main

fn main(console: Console)
  console.print("#{Unit.compare(Unit, Unit)} #{Unit.compare(Unit)}")
  console.print("#{Unit.to_text} #{Unit.to_text(Unit)} #{Unit.compare(Unit, Unit).to_text}")
  order = Unit.compare
  console.print("#{order(Unit, Unit)}")
  console.print("#{Equal.to_text} #{Less.compare(Greater)}")
end

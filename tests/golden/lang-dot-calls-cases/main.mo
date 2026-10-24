module Main

enum Light
  Red
  Amber
  Green
end

fn next(light: Light) : Light
  case light
    Red: Green
    Amber: Red
    Green: Amber
  end
end

fn wait(light: Light, seconds: Int) : Text
  "#{light} for #{seconds}s"
end

fn main(console: Console)
  console.print(Equal.to_text)
  console.print(Less.compare(Greater).to_text)
  console.print("#{Greater.compare(Less)} #{Equal.compare(Equal)}")
  console.print(Red.next.to_text)
  console.print(Amber.next.next.to_text)
  console.print(Green.wait(30))
  console.print("#{Red.compare(Green)} #{Green.compare(Amber)}")
end

module Main

struct Point
  x: Decimal
  y: Int
end

enum Mark
  Plain
  Named(label: Text)
end

fn main(console: Console)
  console.print("#{Point(x: 1.5, y: 2)}")
  console.print("#{Some(3)} #{[1, 2].get(5)} #{[1, 2]} #{(1, true)}")
  console.print("#{Named(label: "q\"uote")} #{Plain}")
  console.print("#{["a", "b"]} #{'c'} #{"plain"}")
  console.print("#{0..3} #{1000.0} #{0.25} #{-0.5}")
end

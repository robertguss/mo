module Main
use Shapes{Shape}

struct Point
  x: Int
  y: Int
end

by_text = Text.compare

fn largest(a: T, b: T) : T where T: Order
  if T.compare(a, b) == Less: b else: a
end

fn label(value: T) : Text where T: Text
  "<#{T.to_text(value)}>"
end

fn parse(text: Text) : Result(Int, Text)
  text.to_int.or_fail("not a number: #{text}")
end

fn main(console: Console)
  console.print("#{Int.compare(3, 7)} #{Bool.compare(true, false)} #{Char.compare('a', 'a')}")
  quoted = Text.to_text("say \"hi\"")
  console.print("#{quoted} #{Float.to_text(0.25.to_float)} #{Decimal.compare(1.50, 1.5)}")
  console.print("#{List.to_text(["a", "b"])} #{List.compare([1, 2], [1, 2, 0])}")
  console.print("#{Map.to_text({"k": [1]})} #{Set.to_text(Set.empty.add(2))}")
  found = [4, 5].first
  missing = [4, 5].get(9)
  console.print("#{Option.to_text(found)} #{Option.compare(missing, found)}")
  console.print("#{Result.to_text(parse("12"))} #{Result.to_text(parse("x"))}")
  console.print("#{Result.compare(parse("1"), parse("x"))}")
  console.print("#{Ordering.to_text(Greater)} #{Ordering.compare(Greater, Equal)}")
  p = Point(x: 1, y: 2)
  console.print("#{Point.to_text(p)} #{Point.compare(p, Point(x: 1, y: 3))}")
  console.print("#{Shapes.to_text(Square(side: 2))} #{Shape.to_text(Circle(radius: 1))}")
  console.print("#{Shapes.compare(Square(side: 1), Circle(radius: 5))}")
  cheap = Shapes.money(5)
  dear = Shapes.money(70)
  console.print("#{Shapes.to_text(cheap)} #{Shapes.compare(cheap, dear)} #{cheap < dear}")
  console.print("#{largest(cheap, dear)} #{largest(Square(side: 1), Circle(radius: 9))}")
  console.print(label([Less]))
  by_int = Int.compare
  shapes = [Square(side: 1), Circle(radius: 2)]
  console.print("#{by_int(2, 1)} #{by_text("b", "a")} #{shapes.map(Shape.to_text).join(" | ")}")
  console.print("#{[Less, Greater].map(Ordering.to_text)}")
  order = Shapes.compare
  show = Shapes.to_text
  texts = [found, missing].map(Option.to_text)
  console.print("#{order(dear, cheap)} #{show(dear)} #{texts} #{[[1], []].map(List.to_text)}")
end

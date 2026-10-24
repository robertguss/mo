module Main

struct Tagged
  tags: Set(Text)
  spots: Map(Text, (Int, Bool))
  span: Range
end

struct Blob
  data: Bytes
  size: Int
end

enum Shape
  Dot((Int, Int))
  Path(List(Option(Decimal)))
  Empty
end

fn main(console: Console)
  one = Tagged(tags: ["b", "a"].to_set, spots: {"x": (1, true)}, span: 0..3)
  two = Tagged(tags: ["a", "b"].to_set, spots: {"x": (1, true)}, span: 0..3)
  console.print("#{one == two} #{{one: "first"}.get(two)} #{[one, two].unique.size}")
  console.print("#{one}")
  blobs = [Blob(data: "b".bytes, size: 1), Blob(data: "a".bytes, size: 2)]
  console.print("#{blobs.sort.map(fn(blob) blob.size end)} #{blobs.to_set.size}")
  shapes = [Empty, Path([Some(1.5), None]), Dot((2, 1)), Path([None]), Dot((1, 9))]
  console.print("#{shapes.sort}")
  console.print("#{[(false, Unit), (true, Unit), (false, Unit)].sort}")
  console.print("#{{(1.5, 'a'): Path([]), (1.5, 'b'): Empty}.get((1.5, 'b'))}")
  console.print("#{[[1, 2, 0], [1, 2], [0, 9]].sort} #{[2.seconds, 1.seconds].min}")
end

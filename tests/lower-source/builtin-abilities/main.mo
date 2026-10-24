module Main

comparison = Int.compare
initial = comparison(7, -2)

struct Label
  value: Int
end

fn to_text(label: Label) : Text
  "<#{label.value}>"
end

fn render(value: T) : Text where T: Text
  value.to_text
end

fn render_map(map: Map(K, V)) : Text where K: Hash, K: Text, V: Text
  map.to_text
end

fn order(a: T, b: T) : Ordering where T: Order
  a.compare(b)
end

fn qualified(a: T, b: T) : Ordering where T: Order
  T.compare(a, b)
end

fn empty(map: Map(Text, Int)) : Text
  map.to_text
end

fn main(console: Console) : Int
  less = Less
  equal = Equal
  greater = Greater
  entry = Entry(name: "a\n\"λ", kind: Folder)
  map = {"b": Some(Less), "a": None}
  console.print("#{less}/#{equal.to_text}/#{render(greater)}")
  console.print("#{entry}")
  console.print("#{map}")
  console.print("#{empty({})}/#{[(File, Link), (Other, Folder)]}")
  console.print("#{less.compare(Greater)}/#{order(Greater, Equal)}/#{initial}")
  textify = Ordering.to_text
  if render(Less) == "Less" and
    render(Equal) == "Equal" and
    greater.to_text == "Greater" and
    entry.to_text == "Entry(name: \"a\\n\\\"λ\", kind: Folder)" and
    render(map) == "{\"b\": Some(Less), \"a\": None}" and
    empty({}) == "{}" and
    {2: "x\n", -1: "y"}.to_text == "{2: \"x\\n\", -1: \"y\"}" and
    render_map({Label(value: 7): Label(value: 3)}) == "{<7>: <3>}" and
    render_map({'\n': '\''}) == "{'\\n': '\\''}" and
    map.set("a", Some(Greater)).remove("b").set("b", Some(Equal)).to_text ==
      "{\"a\": Some(Greater), \"b\": Some(Equal)}" and
    [{"x": [1, 2]}].to_text == "[{\"x\": [1, 2]}]" and
    less.compare(Equal) == Less and
    equal.compare(Less) == Greater and
    greater.compare(Greater) == Equal and
    order(Less, Greater) == Less and
    Less < Equal and
    Equal <= Equal and
    Greater > Equal and
    Greater >= Less and
    order(File, Folder) == Less and
    order(entry, Entry(name: "z", kind: File)) == Less and
    Int.compare(-2, 7) == Less and
    Ordering.compare(Less, Greater) == Less and
    Option.compare(None, Some(1)) == Less and
    qualified(entry, Entry(name: "z", kind: File)) == Less and
    Text.to_text("x\n") == "x\n" and
    List.to_text([Less]) == "[Less]" and
    Label.to_text(Label(value: 7)) == "<7>" and
    [Label(value: 1)].map(Label.to_text) == ["<1>"] and
    textify(Equal) == "Equal" and
    comparison(7, 7) == Equal and
    initial == Greater and
    Int.compare(-9223372036854775808, 9223372036854775807) == Less: 0 else: 1
end

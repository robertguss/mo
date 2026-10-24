module Main

struct Label
  value: Int
end

fn to_text(label: Label) : Text
  "label #{label.value}"
end

fn convert(items: List(T)) : Set(T) where T: Hash
  items.to_set
end

fn apply(f: fn(List(Int)) : Set(Int), items: List(Int)) : Set(Int)
  f(items)
end

fn render(value: T) : Text where T: Text
  value.to_text
end

fn main(console: Console) : Int
  values = [3, 1, 3, 2, 1]
  original = values
  convert_ints = List.to_set
  set = convert(values)
  assert original == [3, 1, 3, 2, 1]
  assert set.to_list == [3, 1, 2]
  assert List.to_set(values).to_list == [3, 1, 2]
  assert convert_ints(values).to_list == [3, 1, 2]
  assert apply(List.to_set, values).to_list == [3, 1, 2]
  assert apply(convert_ints, []).empty?
  assert convert(["b", "a", "b"]).to_list == ["b", "a"]
  console.print(render(set))
  console.print("#{Set.empty.add(0).remove(0)}")
  console.print(["a\n", "\"\\", "é"].to_set.to_text)
  console.print(['\n', '\'', 'é'].to_set.to_text)
  console.print([[2, 1].to_set, Set.empty].to_set.to_text)
  console.print(Some(set).to_text)
  console.print([Label(value: 7), Label(value: 2)].to_set.to_text)
  assert render(set) == "[3, 1, 2].to_set"
  assert ["a\n", "\"\\", "é"].to_set.to_text == "[\"a\\n\", \"\\\"\\\\\", \"é\"].to_set"
  assert ['\n', '\'', 'é'].to_set.to_text == "['\\n', '\\'', 'é'].to_set"
  assert [[2, 1].to_set, Set.empty].to_set.to_text == "[[2, 1].to_set, [].to_set].to_set"
  assert Some(set).to_text == "Some([3, 1, 2].to_set)"
  assert [Label(value: 7), Label(value: 2)].to_set.to_text == "[label 7, label 2].to_set"
  0
end

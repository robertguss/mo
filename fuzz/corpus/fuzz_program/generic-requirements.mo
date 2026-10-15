module Main

struct Bag(T)
  values: Set(T)
end

fn same(a: A, b: B) : Bool where A: Equal, B: Equal
  a == a and b == b
end

fn main() : Int
  bag = Bag(values: Set.empty.add(1))
  case bag
    Bag(values: _): if same(1, 2): 3 else: 4
  end
end

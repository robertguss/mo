module Main

enum Tree(T)
  Leaf
  Node(left: Tree(T), value: T, right: Tree(T))
end

enum Either(L, R)
  Left(L)
  Right(R)
end

fn insert(tree: Tree(Int), value: Int) : Tree(Int)
  case tree
    Leaf: Node(left: Leaf, value: value, right: Leaf)
    Node(left: l, value: v, right: r):
      if value < v
        Node(left: insert(l, value), value: v, right: r)
      else
        Node(left: l, value: v, right: insert(r, value))
      end
  end
end

fn size(tree: Tree(T)) : Int
  case tree
    Leaf: 0
    Node(left: l, right: r): size(l) + 1 + size(r)
  end
end

fn in_order(tree: Tree(T)) : List(T)
  case tree
    Leaf: []
    Node(left: l, value: v, right: r): in_order(l).append(v).concat(in_order(r))
  end
end

fn map_tree(tree: Tree(T), f: fn(T) : U) : Tree(U)
  case tree
    Leaf: Leaf
    Node(left: l, value: v, right: r): Node(
      left: map_tree(l, f),
      value: f(v),
      right: map_tree(r, f)
    )
  end
end

fn describe(value: Either(L, R)) : Text where L: Text, R: Text
  case value
    Left(l): "left #{l}"
    Right(r): "right #{r}"
  end
end

fn first_or(items: List(T), fallback: T) : T
  case items.first
    Some(item): item
    None: fallback
  end
end

fn main(console: Console) : Int
  tree = [5, 2, 8, 1].reduce(Leaf, insert)
  labels = map_tree(tree, fn(n) "v#{n}" end)
  shown = [Left(1), Right("b")].map(describe)
  found = first_or([9], 0)
  fallback = first_or([], "none")
  console.print("#{size(tree)}/#{in_order(tree)}/#{in_order(labels)}/#{shown}/#{found}/#{fallback}")
  if size(tree) == 4 and
    in_order(tree) == [1, 2, 5, 8] and
    in_order(labels) == ["v1", "v2", "v5", "v8"] and
    shown == ["left 1", "right b"] and
    found == 9 and
    fallback == "none": 0 else: 1
end

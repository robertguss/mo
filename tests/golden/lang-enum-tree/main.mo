module Main

enum Tree(T)
  Leaf
  Node(left: Tree(T), value: T, right: Tree(T))
end

fn insert(tree: Tree(Int), n: Int) : Tree(Int)
  case tree
    Leaf: Node(left: Leaf, value: n, right: Leaf)
    Node(left: l, value: v, right: r):
      if n < v
        Node(left: insert(l, n), value: v, right: r)
      else
        Node(left: l, value: v, right: insert(r, n))
      end
  end
end

fn walk(tree: Tree(Int)) : List(Int)
  case tree
    Leaf: []
    Node(left: l, value: v, right: r): walk(l).append(v).concat(walk(r))
  end
end

fn depth(tree: Tree(Int)) : Int
  case tree
    Leaf: 0
    Node(left: l, value: _, right: r):
      dl = depth(l)
      dr = depth(r)
      1 + (if dl > dr: dl else: dr)
  end
end

fn main(console: Console)
  var tree = Leaf
  for n in [5, 2, 8, 1, 9, 3]
    tree = insert(tree, n)
  end
  console.print("#{walk(tree)}")
  console.print("#{depth(tree)}")
end

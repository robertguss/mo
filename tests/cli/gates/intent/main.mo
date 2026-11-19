module Main
expose Box(..), Crate(..), size, bare, also_bare, main

struct Box
  width: Int
end

struct Crate
  depth: Int
end

fn size(box: Box) : Int
  examples
    size(Box(width: 2)) == 2
  end

  box.width
end

fn size(crate: Crate) : Int
  crate.depth
end

fn bare(n: Int) : Int
  helper(n) + 1
end

fn also_bare(n: Int) : Bool
  n > 0
end

fn helper(n: Int) : Int
  n * 2
end

test "bare adds one to double"
  assert bare(1) == 3
  assert also_bare(1)
  assert Crate(depth: 3).size == 3
end

fn main()
end

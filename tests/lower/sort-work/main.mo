module Main

## Input shapes for test_source_sort_work: 0 scrambled, 1 already sorted, 2 reversed, 3 all equal.
fn input(n: Int, shape: Int) : List(Int)
  (0..n).to_list.map(fn(i) value(i, n, shape) end)
end

fn value(i: Int, n: Int, shape: Int) : Int
  if shape == 0
    (i * 2654435761) % 4294967296 / 65536
  else if shape == 1
    i
  else if shape == 2
    n - i
  else
    7
  end
end

## The input sorted by way: 0 sort, 1 sort_by, 2 sort_by_desc.
fn sorted(n: Int, shape: Int, way: Int) : List(Int)
  items = input(n, shape)
  if way == 0
    items.sort
  else if way == 1
    items.sort_by(fn(x) x end)
  else
    items.sort_by_desc(fn(x) x end)
  end
end

fn main() : Int
  sorted(8, 0, 0).size
end

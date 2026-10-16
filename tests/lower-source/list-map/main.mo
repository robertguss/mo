module Main

doubled = [4, 5].map(bump)
base = 3

fn bump(value: Int) : Int
  value + base
end

fn empty_ints() : List(Int)
  []
end

fn main(console: Console) : Int
  empty = empty_ints().map(fn(x)
    x + 1
  end)
  dots = [1, 2, 3].map(fn(x)
    x * 2
  end)
  lengths = List.map(["a", "bc", "def"], Text.byte_count)
  mapper = List.map
  shifted = mapper(
    [3, 4],
    fn(n)
      n + 10
    end
  )
  offset = 5
  guarded = [10, 20].map(fn(n)
    return 0 if n == 10
    if n < 0: stop "negative" else: n + offset
  end)
  console.print("#{dots}/#{lengths}/#{shifted}/#{guarded}/#{doubled}")
  if empty == [] and
    dots == [2, 4, 6] and
    lengths == [1, 2, 3] and
    shifted == [13, 14] and
    guarded == [0, 25] and
    doubled == [7, 8]: 0 else: 1
end

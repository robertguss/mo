module Main

positive_count = [3, -1, 4, 0, 5].count(above_threshold)
threshold = 0

fn above_threshold(value: Int) : Bool
  value > threshold
end

fn empty_ints() : List(Int)
  []
end

fn main(console: Console) : Int
  empty = empty_ints().count(fn(x)
    x > 0
  end)
  none = [1, 2, 3].count(fn(x)
    x > 10
  end)
  all = [1, 2, 3].count(fn(x)
    x > 0
  end)
  newlines = "one two\nthree\n".chars.count(fn(c)
    c == '\n'
  end)
  qualified = List.count(
    ["a", "", "bc", ""],
    fn(s)
      s.byte_count > 0
    end
  )
  counter = List.count
  via_value = counter(
    [10, 20, 30],
    fn(n)
      return true if n == 10
      if n < 0: stop "negative" else: n == 30
    end
  )
  console.print("#{empty}/#{none}/#{all}/#{newlines}/#{qualified}/#{via_value}/#{positive_count}")
  if empty == 0 and
    none == 0 and
    all == 3 and
    newlines == 2 and
    qualified == 2 and
    via_value == 2 and
    positive_count == 3: 0 else: 1
end

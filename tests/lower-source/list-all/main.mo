module Main

all_positive = [3, 4, 5].all?(above_threshold)
threshold = 0

fn above_threshold(value: Int) : Bool
  value > threshold
end

fn empty_ints() : List(Int)
  []
end

fn main(console: Console) : Int
  empty = empty_ints().all?(fn(x)
    x > 0
  end)
  all_true = [1, 2, 3].all?(fn(x)
    x > 0
  end)
  some_false = [1, 2, 3].all?(fn(x)
    x < 3
  end)
  short_circuit = [1, -1, 2].all?(fn(x)
    if x == 2: stop "reached after false" else: x > 0
  end)
  qualified = List.all?(
    ["a", "bc"],
    fn(s)
      s.byte_count > 0
    end
  )
  every = List.all?
  via_value = every(
    [10, 20, 30],
    fn(n)
      return true if n == 10
      if n < 0: stop "negative" else: n >= 20
    end
  )
  console.print(
    "#{empty}/#{all_true}/#{some_false}/#{short_circuit}/#{qualified}/#{via_value}/#{all_positive}"
  )
  if empty and
    all_true and
    not some_false and
    not short_circuit and
    qualified and
    via_value and
    all_positive: 0 else: 1
end

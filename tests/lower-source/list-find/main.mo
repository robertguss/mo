module Main

first_big = [3, 40, 50].find(above_threshold)
threshold = 10

fn above_threshold(value: Int) : Bool
  value > threshold
end

fn empty_ints() : List(Int)
  []
end

fn show(found: Option(Int)) : Text
  case found
    None: "none"
    Some(n): "#{n}"
  end
end

fn show_text(found: Option(Text)) : Text
  case found
    None: "none"
    Some(t): t
  end
end

fn main(console: Console) : Int
  empty = empty_ints().find(fn(x)
    x > 0
  end)
  hit = [1, 2, 3].find(fn(x)
    x > 1
  end)
  miss = [1, 2, 3].find(fn(x)
    x > 5
  end)
  short_circuit = [1, 2, 3].find(fn(x)
    if x == 3: stop "reached after a hit" else: x == 2
  end)
  qualified = List.find(
    ["a", "bc", "def"],
    fn(s)
      s.byte_count > 1
    end
  )
  finder = List.find
  via_value = finder(
    [10, 20, 30],
    fn(n)
      return false if n == 10
      if n < 0: stop "negative" else: n >= 20
    end
  )
  console.print(
    "#{show(empty)}/#{show(hit)}/#{show(miss)}/#{show(short_circuit)}/#{show_text(qualified)}/#{show(via_value)}/#{show(first_big)}"
  )
  if empty == None and
    hit == Some(2) and
    miss == None and
    short_circuit == Some(2) and
    qualified == Some("bc") and
    via_value == Some(20) and
    first_big == Some(40): 0 else: 1
end

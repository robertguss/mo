module Main

fn total(items: List(Int)) : Int
  examples
    total([]) == 0
    total([1, 2, 3]) == 6
  end

  items.reduce_total
end

fn reduce_total(items: List(Int)) : Int
  var sum = 0
  for n in items
    sum += n
  end
  sum
end

test "reversing twice gives the list back"
  items = [1, 2, 3]
  assert items.reverse.reverse == items
end

test "a map keeps insertion order"
  m = {"b": 1, "a": 2}
  assert m.keys == ["b", "a"]
end

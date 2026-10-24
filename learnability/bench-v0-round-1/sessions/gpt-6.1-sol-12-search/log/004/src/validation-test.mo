module ValidationTest
use Search

test "all searches match linear results on short sorted lists"
  for length in 0..5
    for encoding in 0..5.pow(length)
      var digits = encoding
      var values = []
      for _ in 0..length
        values = values.append(digits % 5 - 2)
        digits = digits / 5
      end
      items = values.sort
      for x in -3..4
        assert Search.lower_bound(items, x) == items.count(fn(item) item < x end)
        expected_index = items.enumerate.find(fn(pair) pair.1 == x end)
          .map(fn(pair) pair.0 end)
        assert Search.index_of(items, x) == expected_index
        for high in -3..4
          expected_count = items.count(fn(item) item >= x and item < high end)
          assert Search.count_between(items, x, high) == expected_count
        end
      end
    end
  end
end

test "searching integer extremes does not need target arithmetic"
  smallest = -9223372036854775808
  largest = 9223372036854775807
  lists = [[], [smallest], [largest], [smallest, smallest, -1, 0, largest, largest]]
  targets = [smallest, smallest + 1, -1, 0, 1, largest - 1, largest]
  for items in lists
    for x in targets
      assert Search.lower_bound(items, x) == items.count(fn(item) item < x end)
      expected_index = items.enumerate.find(fn(pair) pair.1 == x end)
        .map(fn(pair) pair.0 end)
      assert Search.index_of(items, x) == expected_index
      for high in targets
        expected_count = items.count(fn(item) item >= x and item < high end)
        assert Search.count_between(items, x, high) == expected_count
      end
    end
  end
end

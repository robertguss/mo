module Main

struct Item
  rank: Int
  label: Text
end

struct Score
  points: Int
end

fn compare(left: Score, right: Score) : Ordering
  (left.points + tie_bias).compare(right.points + tie_bias)
end

ranked_scores = [Score(points: 2), Score(points: 5), Score(points: 3)].sort_by_desc(score_key)
tie_bias = 0

fn score_key(score: Score) : Score
  score
end

fn score_points(score: Score) : Int
  score.points
end

fn bump_key(table: Map(K, Int), key: K, step: Int) : Map(K, Int) where K: Hash
  table.update(
    key,
    0,
    fn(current)
      current + step
    end
  )
end

fn top_down(values: List(T), by: fn(T) : K) : List(T) where K: Order
  values.sort_by_desc(by)
end

fn empty_items() : List(Item)
  []
end

fn main(console: Console) : Int
  hit = {"a": 1}.update(
    "a",
    0,
    fn(n)
      n + 1
    end
  )
  miss = {"a": 1}.update(
    "b",
    0,
    fn(n)
      n + 1
    end
  )
  updater = Map.update
  via_value = updater(
    miss,
    "b",
    10,
    fn(n)
      return 99 if n < 0
      if n > 100: stop "overflow" else: n + 4
    end
  )
  generic_map = bump_key(via_value, "c", 7)
  sorted = [1, 3, 2].sort_by_desc(fn(x)
    x
  end)
  sorter = List.sort_by_desc
  by_len = sorter(["a", "abcd", "bc", "def"], Text.byte_count)
  stable_input = [
    Item(rank: 1, label: "first"),
    Item(rank: 3, label: "top-a"),
    Item(rank: 2, label: "mid-a"),
    Item(rank: 3, label: "top-b"),
    Item(rank: 1, label: "second"),
    Item(rank: 2, label: "mid-b"),
    Item(rank: 1, label: "third")
  ]
  stable_sorted = top_down(
    stable_input,
    fn(item)
      item.rank
    end
  )
  stable_labels = stable_sorted.map(fn(item)
    item.label
  end)
  empty_sorted = empty_items().sort_by_desc(fn(item)
    item.rank
  end)
  single_sorted = [42].sort_by_desc(fn(x)
    x
  end)
  score_list = ranked_scores.map(score_points)
  console.print(
    "#{hit.get("a") or 0}/#{miss.get("a") or 0},#{miss.get("b") or 0}/#{generic_map.get("b") or 0},#{generic_map
      .get("c") or
      0}/#{sorted}/#{by_len}/#{stable_labels}/#{score_list}"
  )
  if hit == {"a": 2} and
    miss == {"a": 1, "b": 1} and
    generic_map == {"a": 1, "b": 5, "c": 7} and
    sorted == [3, 2, 1] and
    by_len == ["abcd", "def", "bc", "a"] and
    stable_labels == ["top-a", "top-b", "mid-a", "mid-b", "first", "second", "third"] and
    empty_sorted == [] and
    single_sorted == [42] and
    score_list == [5, 3, 2]: 0 else: 1
end

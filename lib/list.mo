module List
expose map,
filter,
filter_map,
flat_map,
reduce,
find,
any?,
all?,
count,
sort_by,
sort_by_desc,
sort,
min,
max,
group_by

## The results of f on each element, in order.
fn map(items: List(T), f: fn(T) : U) : List(U)
  examples
    [1, 2, 3].map(fn(n) n * 2 end) == [2, 4, 6]
    ["a", "bc"].map(fn(t) t.byte_count end) == [1, 2]
    [1].drop(1).map(fn(n) n + 1 end) == []
  end

  var mapped = []
  for item in items
    mapped = mapped.append(f(item))
  end
  mapped
end

## The elements for which pred is true, in order.
fn filter(items: List(T), pred: fn(T) : Bool) : List(T)
  examples
    [1, 2, 3, 4].filter(fn(n) n % 2 == 0 end) == [2, 4]
    [1].drop(1).filter(fn(n) n > 0 end) == []
  end

  var kept = []
  for item in items
    if pred(item)
      kept = kept.append(item)
    end
  end
  kept
end

## The values in the Some results of f, in order.
fn filter_map(items: List(T), f: fn(T) : Option(U)) : List(U)
  examples
    ["1", "x", "3"].filter_map(fn(t) t.to_int end) == [1, 3]
  end

  var kept = []
  for item in items
    case f(item)
      Some(value): kept = kept.append(value)
      None: Unit
    end
  end
  kept
end

## The elements of every list f gives, joined in order.
fn flat_map(items: List(T), f: fn(T) : List(U)) : List(U)
  examples
    [1, 2].flat_map(fn(n) [n, n * 10] end) == [1, 10, 2, 20]
  end

  var joined = []
  for item in items
    joined = joined.concat(f(item))
  end
  joined
end

## Folds the list from the left, starting from start.
fn reduce(items: List(T), start: A, f: fn(A, T) : A) : A
  examples
    [1, 2, 3].reduce(0, fn(sum, n) sum + n end) == 6
    ["a", "b"].reduce("", fn(text, t) "#{text}#{t}" end) == "ab"
  end

  var total = start
  for item in items
    total = f(total, item)
  end
  total
end

## Whether pred is true for at least one element; it stops at the first.
fn any?(items: List(T), pred: fn(T) : Bool) : Bool
  examples
    [1, 2, 3].any?(fn(n) n > 2 end)
    not [1, 2].any?(fn(n) n > 2 end)
    not [1].drop(1).any?(fn(_n) true end)
  end

  for item in items
    return true if pred(item)
  end
  false
end

## The elements ordered by key, smallest first; equal keys keep their order.
fn sort_by(items: List(T), key: fn(T) : K) : List(T) where K: Order
  examples
    ["ccc", "a", "bb"].sort_by(fn(t) t.byte_count end) == ["a", "bb", "ccc"]
    [(1, "b"), (0, "z"), (1, "a")].sort_by(fn(p) p.0 end) == [(0, "z"), (1, "b"), (1, "a")]
  end

  keyed = items.map(fn(item) (key(item), item) end)
  sorted = sort_keyed(keyed)
  sorted.map(fn(pair) pair.1 end)
end

## The elements in order, smallest first; equal elements keep their order.
fn sort(items: List(T)) : List(T) where T: Order
  examples
    [3, 1, 2].sort == [1, 2, 3]
    ["b", "a"].sort == ["a", "b"]
    [1].drop(1).sort == []
    [2.0, 1.0].sort == [1.0, 2.0]
  end

  keyed = items.map(fn(item) (item, item) end)
  sorted = sort_keyed(keyed)
  sorted.map(fn(pair) pair.1 end)
end

## Merge sort on the first part of each pair. It is stable: on equal keys the left list goes first.
fn sort_keyed(pairs: List((K, T))) : List((K, T)) where K: Order
  size = pairs.size
  if size < 2
    pairs
  else
    half = size / 2
    merge_keyed(sort_keyed(pairs.take(half)), sort_keyed(pairs.drop(half)))
  end
end

fn merge_keyed(left: List((K, T)), right: List((K, T))) : List((K, T)) where K: Order
  var merged = []
  var i = 0
  var j = 0
  while i < left.size and j < right.size
    case (left.get(i), right.get(j))
      (Some(a), Some(b)):
        if b.0.compare(a.0) == Less
          merged = merged.append(b)
          j += 1
        else
          merged = merged.append(a)
          i += 1
        end
      _: Unit
    end
  end
  merged.concat(left.drop(i)).concat(right.drop(j))
end

## The smallest element, or the first of several equal ones; None for an empty list.
fn min(items: List(T)) : Option(T) where T: Order
  examples
    [3, 1, 2].min == Some(1)
    [1].drop(1).min == None
    [(1, "a"), (1, "b")].min == Some((1, "a"))
  end

  var best = None
  for item in items
    case best
      Some(current):
        if item.compare(current) == Less
          best = Some(item)
        end
      None: best = Some(item)
    end
  end
  best
end

## The elements by the key f gives, each key's elements in order, keys in the order they first appear.
fn group_by(items: List(T), key: fn(T) : K) : Map(K, List(T))
  examples
    [1, 2, 3, 4].group_by(fn(n) n % 2 end) == {1: [1, 3], 0: [2, 4]}
    [1].drop(1).group_by(fn(n) n % 2 end) == {}
  end

  var groups = {}
  for item in items
    k = key(item)
    members = groups.get(k) or []
    groups = groups.set(k, members.append(item))
  end
  groups
end

## The first element for which pred is true, or None; it stops there.
fn find(items: List(T), pred: fn(T) : Bool) : Option(T)
  examples
    [1, 2, 3].find(fn(n) n > 1 end) == Some(2)
    [1, 2, 3].find(fn(n) n > 5 end) == None
  end

  for item in items
    return Some(item) if pred(item)
  end
  None
end

## Whether pred is true for every element; it stops at the first that isn't.
fn all?(items: List(T), pred: fn(T) : Bool) : Bool
  examples
    [2, 4].all?(fn(n) n % 2 == 0 end)
    not [2, 3].all?(fn(n) n % 2 == 0 end)
    [1].drop(1).all?(fn(n) n > 100 end)
  end

  for item in items
    return false if not pred(item)
  end
  true
end

## How many elements pred is true for.
fn count(items: List(T), pred: fn(T) : Bool) : Int
  examples
    [1, 2, 3, 4].count(fn(n) n % 2 == 0 end) == 2
    [1].drop(1).count(fn(n) n > 0 end) == 0
  end

  var total = 0
  for item in items
    if pred(item)
      total += 1
    end
  end
  total
end

## The elements ordered by key, largest first; equal keys keep their order.
fn sort_by_desc(items: List(T), key: fn(T) : K) : List(T) where K: Order
  examples
    ["a", "ccc", "bb"].sort_by_desc(fn(t) t.byte_count end) == ["ccc", "bb", "a"]
    [(1, "b"), (2, "z"), (1, "a")].sort_by_desc(fn(p) p.0 end) == [(2, "z"), (1, "b"), (1, "a")]
  end

  keyed = items.map(fn(item) (key(item), item) end)
  sorted = sort_keyed_desc(keyed)
  sorted.map(fn(pair) pair.1 end)
end

## Merge sort on the first part of each pair, largest first. On equal keys the left list goes first.
fn sort_keyed_desc(pairs: List((K, T))) : List((K, T)) where K: Order
  size = pairs.size
  if size < 2
    pairs
  else
    half = size / 2
    merge_keyed_desc(sort_keyed_desc(pairs.take(half)), sort_keyed_desc(pairs.drop(half)))
  end
end

fn merge_keyed_desc(left: List((K, T)), right: List((K, T))) : List((K, T)) where K: Order
  var merged = []
  var i = 0
  var j = 0
  while i < left.size and j < right.size
    case (left.get(i), right.get(j))
      (Some(a), Some(b)):
        if b.0.compare(a.0) == Greater
          merged = merged.append(b)
          j += 1
        else
          merged = merged.append(a)
          i += 1
        end
      _: Unit
    end
  end
  merged.concat(left.drop(i)).concat(right.drop(j))
end

## The largest element, or the first of several equal ones; None for an empty list.
fn max(items: List(T)) : Option(T) where T: Order
  examples
    [3, 1, 2].max == Some(3)
    [1].drop(1).max == None
    [(1, "a"), (0, "b")].max == Some((1, "a"))
  end

  var best = None
  for item in items
    case best
      Some(current):
        if item.compare(current) == Greater
          best = Some(item)
        end
      None: best = Some(item)
    end
  end
  best
end

test "sort_by keeps the order of equal keys"
  words = ["bb", "a", "cc", "d", "ee"]
  assert words.sort_by(fn(w) w.byte_count end) == ["a", "d", "bb", "cc", "ee"]
end

test "sort puts not-a-number last"
  assert "nan".to_float is Some(not_a_number)
  sorted = [not_a_number, 2.0, 1.0].sort
  assert sorted.take(2) == [1.0, 2.0]
  assert sorted.last is Some(biggest)
  assert biggest != biggest
end

test "sort handles a long list"
  numbers = (0..500).to_list.map(fn(i) (i * 37) % 101 end)
  sorted = numbers.sort
  assert sorted.size == 500
  assert sorted.first == Some(0)
  assert sorted.last == Some(100)
  assert sorted == sorted.sort
  assert sorted.reverse.sort == sorted
end

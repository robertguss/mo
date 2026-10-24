## Set's operations of reference sections 3.8, 9.2 and 14. The operations are C primitives;
## lowering derives the text form, including each member's own text form.
module Set

test "empty is a constant with no members"
  empty = [1].to_set.remove(1)
  assert Set.empty == empty
  assert Set.empty.to_list == [1].take(0)
  assert empty.size == 0
  assert empty.empty?
  assert not empty.has?(1)
end

test "to_set keeps distinct members in their first insertion order"
  values = [3, 1, 3, 2, 1]
  set = values.to_set
  assert set.to_list == [3, 1, 2]
  assert set.size == 3
  assert not set.empty?
  assert values == [3, 1, 3, 2, 1]
  assert ["b", "é", "a", "b"].to_set.to_list == ["b", "é", "a"]
  assert [true, false, true].to_set.to_list == [true, false]
end

test "has? distinguishes members from absent values"
  set = [3, 1, 2].to_set
  assert set.has?(3)
  assert set.has?(1)
  assert set.has?(2)
  assert not set.has?(0)
  assert not ["é"].to_set.has?("e\u{301}")
end

test "add appends new members and leaves existing members in place"
  set = Set.empty.add(3).add(1).add(2)
  assert set.to_list == [3, 1, 2]
  assert set.add(1).to_list == [3, 1, 2]
  assert set.add(4).to_list == [3, 1, 2, 4]
  assert set.add(3).size == 3
  assert set.to_list == [3, 1, 2]
end

test "remove keeps the remaining order and readding goes last"
  set = [3, 1, 2].to_set
  assert set.remove(1).to_list == [3, 2]
  assert set.remove(3).to_list == [1, 2]
  assert set.remove(2).to_list == [3, 1]
  assert set.remove(4).to_list == [3, 1, 2]
  assert set.remove(3).add(3).to_list == [1, 2, 3]
  assert set.remove(3).remove(1).remove(2).empty?
  assert set.to_list == [3, 1, 2]
end

test "union appends only new members in the other set's order"
  left = [3, 1].to_set
  right = [2, 1, 4].to_set
  assert left.union(right).to_list == [3, 1, 2, 4]
  assert right.union(left).to_list == [2, 1, 4, 3]
  assert left.union(left).to_list == [3, 1]
  assert left.union(Set.empty).to_list == [3, 1]
  assert Set.empty.union(right).to_list == [2, 1, 4]
  assert left.to_list == [3, 1]
  assert right.to_list == [2, 1, 4]
end

test "intersection keeps only common members in the first set's order"
  left = [3, 1, 2].to_set
  right = [2, 3, 5].to_set
  assert left.intersection(right).to_list == [3, 2]
  assert right.intersection(left).to_list == [2, 3]
  assert left.intersection(left).to_list == [3, 1, 2]
  assert left.intersection([7].to_set).empty?
  assert left.intersection(Set.empty).empty?
  assert Set.empty.intersection(left).empty?
  assert left.to_list == [3, 1, 2]
  assert right.to_list == [2, 3, 5]
end

test "difference keeps only absent members in the first set's order"
  left = [3, 1, 2].to_set
  right = [2, 5].to_set
  assert left.difference(right).to_list == [3, 1]
  assert right.difference(left).to_list == [5]
  assert left.difference(left).empty?
  assert left.difference([7].to_set).to_list == [3, 1, 2]
  assert left.difference(Set.empty).to_list == [3, 1, 2]
  assert Set.empty.difference(left).empty?
  assert left.to_list == [3, 1, 2]
  assert right.to_list == [2, 5]
end

test "sets are equal by members regardless of insertion order"
  assert [1, 2].to_set == [2, 1].to_set
  assert [1, 2, 1].to_set == [2, 1].to_set
  assert [1, 2].to_set != [1, 3].to_set
  assert [1, 2].to_set != [1, 2, 3].to_set
  assert [1, 2].to_set != Set.empty
end

test "equal sets hash as the same map key or set member"
  left = [1, 2].to_set
  right = [2, 1].to_set
  assert {left: "found"}.get(right) == Some("found")
  assert [left, right, Set.empty].to_set.to_list == [left, Set.empty]
  assert [[1, 2], [2, 1], [1, 2]].to_set.to_list == [[1, 2], [2, 1]]
end

test "equal members keep the first representative"
  first = {1: "a", 2: "b"}
  equal = {2: "b", 1: "a"}
  assert [first, equal].to_set.to_list.map(fn(m) m.keys end) == [[1, 2]]
  assert [first].to_set.add(equal).to_list.map(fn(m) m.keys end) == [[1, 2]]
  assert [first].to_set.union([equal].to_set).to_list.map(fn(m) m.keys end) == [[1, 2]]
end

test "updates preserve an older holder through growth and removals"
  original = [3, 1, 2].to_set
  var current = original
  for n in 4..40
    current = current.add(n)
  end
  current = current.remove(1).remove(20).add(1)
  assert current.to_list == [3, 2].concat((4..20).to_list).concat((21..40).to_list).append(1)
  assert original.to_list == [3, 1, 2]
end

test "iteration visits live members once in insertion order"
  var visited = []
  for n in [3, 1, 3, 2, 4].to_set.remove(1).add(1)
    continue if n == 2
    break if n == 1
    visited = visited.append(n)
  end
  assert visited == [3, 4]
end

test "qualified operations and function values have the same results"
  add = Set.add
  remove = Set.remove
  has = Set.has?
  size = Set.size
  empty = Set.empty?
  union = Set.union
  intersection = Set.intersection
  difference = Set.difference
  to_list = Set.to_list
  set = add(Set.empty, 3)
  assert Set.to_list(Set.add(set, 1)) == [3, 1]
  assert to_list(union(set, [1, 3, 2].to_set)) == [3, 1, 2]
  assert to_list(intersection([2, 3, 1].to_set, set)) == [3]
  assert to_list(difference([2, 3, 1].to_set, set)) == [2, 1]
  assert has(set, 3)
  assert not has(set, 1)
  assert size(set) == 1
  assert empty(remove(set, 3))
end

test "text forms use insertion order and the to_set suffix"
  empty = [1].to_set.remove(1)
  assert empty.to_text == "[].to_set"
  assert [2, 1, 2].to_set.to_text == "[2, 1].to_set"
  assert [1, 2].to_set.to_text == "[1, 2].to_set"
  assert "#{[3, 1, 2].to_set.remove(1).add(1)}" == "[3, 2, 1].to_set"
  assert ["a\n", "\"\\", "é"].to_set.to_text == "[\"a\\n\", \"\\\"\\\\\", \"é\"].to_set"
  assert ['\n', '\'', 'é'].to_set.to_text == "['\\n', '\\'', 'é'].to_set"
  assert [[2, 1].to_set, Set.empty].to_set.to_text == "[[2, 1].to_set, [].to_set].to_set"
  assert Some([2, 1].to_set).to_text == "Some([2, 1].to_set)"
end

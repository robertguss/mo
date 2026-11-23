module Main

struct Inner
  count: Int
end

struct Box
  inner: Inner
  tag: Text
end

fn check_positive(n: Int) : Result(Int, Text)
  if n > 0: Ok(n * 10) else: Error("non-positive")
end

fn validate_both(a: Int, b: Int) : Result(Int, Text)
  _ = try check_positive(a)
  v = try check_positive(b)
  Ok(v)
end

fn emit_and_count(console: Console, label: Text) : Int
  console.print(label)
  42
end

test "Bytes.empty? and Bytes.compare_slice_at"
  assert Bytes.empty.empty?
  assert Bytes.zeroed(0).empty?
  assert not Bytes.zeroed(1).empty?
  buf = Bytes.from_list([0, 1, 127, 128, 255, 16, 32])
  assert not buf.empty?
  assert buf.compare_slice_at(2, 3, Bytes.from_list([127, 128, 255])) == Some(Equal)
  assert buf.compare_slice_at(2, 3, Bytes.from_list([127, 128, 254])) == Some(Greater)
  assert buf.compare_slice_at(2, 3, Bytes.from_list([128, 0, 0])) == Some(Less)
  assert buf.compare_slice_at(3, 1, Bytes.from_list([127])) == Some(Greater)
  assert buf.compare_slice_at(1, 2, Bytes.from_list([1, 127, 0])) == Some(Less)
  assert buf.compare_slice_at(1, 3, Bytes.from_list([1, 127])) == Some(Greater)
  assert buf.compare_slice_at(7, 0, Bytes.empty) == Some(Equal)
  assert buf.compare_slice_at(7, 0, Bytes.from_list([0])) == Some(Less)
  assert Bytes.empty.compare_slice_at(0, 0, Bytes.empty) == Some(Equal)
  assert buf.compare_slice_at(-1, 1, Bytes.empty) == None
  assert buf.compare_slice_at(0, -1, Bytes.empty) == None
  assert buf.compare_slice_at(5, 3, Bytes.empty) == None
  assert buf.compare_slice_at(9223372036854775807, 1, Bytes.empty) == None
  assert buf.compare_slice_at(1, 9223372036854775807, Bytes.empty) == None
end

test "local var loops and field updates inside anonymous functions"
  sum_odds = fn(limit)
    var total = 0
    for i in 0..limit
      continue if i % 2 == 0
      break if i > 7
      total += i
    end
    total
  end
  assert sum_odds(10) == 16

  countdown = fn(start)
    var cur = start
    var steps = 0
    while cur > 0
      cur -= 2
      steps += 1
      return steps if steps == 3
    end
    steps
  end
  assert countdown(10) == 3
  assert countdown(3) == 2

  mutate_box = fn(initial)
    var b = Box(inner: Inner(count: initial), tag: "start")
    snapshot = b
    b.inner.count += 5
    b.tag = "done"
    b.inner.count -= 1
    (snapshot.inner.count, snapshot.tag, b.inner.count, b.tag)
  end
  assert mutate_box(10) == (10, "start", 14, "done")

  nested = fn(base)
    var acc = base
    acc += 3
    reader = fn(delta)
      var local = acc
      local += delta
      local
    end
    reader(4)
  end
  assert nested(5) == 12
end

test "bare discard evaluates once and propagates try"
  console = Console.fake()
  _ = emit_and_count(console, "once")
  _ = if true
    emit_and_count(console, "branch")
  else
    0
  end
  assert console.printed == ["once", "branch"]
  assert validate_both(2, 3) == Ok(30)
  assert validate_both(0, 3) == Error("non-positive")
  assert validate_both(2, -1) == Error("non-positive")
end

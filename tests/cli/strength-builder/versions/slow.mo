module Counter

fn bump(c: Counter) : Counter
  var i = 0
  while i < 200000
    i = i + 1
  end
  Counter(n: c.n + 1)
end

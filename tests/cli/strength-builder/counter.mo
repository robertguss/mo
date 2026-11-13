module Counter
expose Counter, zero, bump, count

struct Counter
  n: Int
end

fn zero() : Counter
  Counter(n: 0)
end

fn bump(c: Counter) : Counter
  Counter(n: c.n + 1)
end

fn count(c: Counter) : Int
  c.n
end

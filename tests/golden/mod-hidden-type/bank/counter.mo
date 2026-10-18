module Bank.Counter
expose Counter, start, bump, value

struct Counter
  count: Int
end

fn start() : Counter
  Counter(count: 0)
end

fn bump(counter: Counter) : Counter
  Counter(count: counter.count + 1)
end

fn value(counter: Counter) : Int
  counter.count
end

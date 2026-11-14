module Main

struct P
  value: Int
end

struct Q
  value: Int
end

fn score(z: P) : Int
  z.value
end

fn score(q: Q) : Int
  q.value
end

fn total(n: Int) : Int
  examples
    total(1) == P(value: 1).score + Q(value: 1).score
  end

  P(value: n).score + Q(value: n).score
end

fn double(n: Int) : Int
  examples
    double(2) == 4
    double(3) == 6
  end

  promise "the result is even"
    result % 2 == 0
    allows n: 1, result: 2
    forbids n: 1, result: 3
  end

  examples
    double(5) == 10
  end

  n * 2
end

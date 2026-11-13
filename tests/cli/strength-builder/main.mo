module Main

use Counter{Counter}

fn total(c: Counter) : Int
  promise "a count is never negative"
    result >= 0
    allows c: Counter.zero(), result: 0
    forbids c: Counter.zero(), result: -1
  end

  examples
    total(Counter.zero()) == 0
  end

  Counter.count(c)
end

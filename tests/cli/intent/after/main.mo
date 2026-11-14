module Main

limit = 10

fn within(n: Int) : Bool
  promise "zero is always within"
    result or n != 0
    allows n: 0, result: true
    forbids n: 0, result: false
  end

  n >= 0 and n <= limit
end

fn clamp(n: Int) : Int
  promise "the result never leaves the limit"
    within(result)
    allows n: 3, result: 3
    forbids n: 20, result: 20
  end

  examples
    clamp(3) == 3
    clamp(20) == limit
    within(3)
  end

  return 0 if n < 0
  return limit if n > limit
  n
end

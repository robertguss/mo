module Main

limit = 10

fn within(n: Int) : Bool
  n >= 0 and n <= limit
end

fn ceiling(n: Int) : Int
  return limit if n > limit
  n
end

fn clamp(n: Int) : Int
  promise "the result stays within the limit"
    within(result)
    allows n: 3, result: 3
    forbids n: 20, result: 20
  end

  examples
    clamp(3) == 3
    clamp(20) == 10
  end

  return 0 if n < 0
  ceiling(n)
end

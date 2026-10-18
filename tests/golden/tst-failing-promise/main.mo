module Main

fn clamp(n: Int) : Int
  promise "the result is never negative"
    result >= 0
    allows n: 5, result: 5
    forbids n: 5, result: -1
  end

  n
end

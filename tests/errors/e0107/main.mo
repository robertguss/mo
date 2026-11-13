module Main

fn double(n: Int) : Int
  promise "doubling never shrinks" on double
    result >= n
    allows n: 1, result: 2
    forbids n: 1, result: 0
  end
  n * 2
end

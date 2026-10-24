module Main

fn double(n: Int) : Int
  promise
    result == n * 2
    allows n: 1, result: 2
    forbids n: 1, result: 3
  end

  n * 2
end

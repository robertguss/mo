module Main

fn bump(n: Int) : Int
  promise "bump adds one"
    result == n + 1
    allows n: 1, result: 2
    forbids n: 1, result: 3
  end

  n + 1
end

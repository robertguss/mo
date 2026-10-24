module Main

fn double(n: Int) : Int
  doubled = n * 2
  promise "the result is twice the input"
    result == n * 2
    allows n: 1, result: 2
    forbids n: 1, result: 3
  end
  doubled
end

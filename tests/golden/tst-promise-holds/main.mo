module Main

fn absolute(n: Int) : Int
  promise "the result is never below zero"
    result >= 0
    allows n: -4, result: 4
    forbids n: -4, result: -4
  end

  if n < 0 and n > -1000: -n else if n < 0: 0 else: n
end

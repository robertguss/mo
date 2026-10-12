# error: a promise has at least one allows line and one forbids line
fn f(x: Int) : Int
  promise "it returns its input"
    result == x
    allows x: 1, result: 1
  end

  x
end

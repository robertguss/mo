# error: unexpected allows
fn f(x: Int) : Int
  promise "it returns its input"
    allows x: 1, result: 1
    forbids x: 1, result: 2
  end

  x
end

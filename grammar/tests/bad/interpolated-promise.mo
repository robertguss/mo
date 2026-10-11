# error: unexpected text before #{, expecting text
fn f(x: Int) : Int
  promise "it returns #{x}"
    result == x
    allows x: 1, result: 1
    forbids x: 1, result: 2
  end

  x
end

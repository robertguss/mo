# error: unexpected text before #{
fn f(x: Int) : Int
  promise "it returns #{x}"
    result == x
  end

  x
end

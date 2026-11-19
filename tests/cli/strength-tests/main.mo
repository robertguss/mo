module Main
expose both, half

fn both(a: Bool, b: Bool) : Bool
  promise "true only when both are"
    result == (a and b)
    allows a: true, b: true, result: true
    forbids a: true, b: false, result: true
  end

  a and b
end

fn half(n: Int) : Int
  n / 2
end

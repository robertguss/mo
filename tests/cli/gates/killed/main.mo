module Main

fn both(a: Bool, b: Bool) : Bool
  promise "true only when both are"
    result == (a and b)
    allows a: true, b: true, result: true
    forbids a: true, b: false, result: true
  end

  a and b
end

fn add(a: Int, b: Int) : Int
  examples
    add(2, 3) == 5
  end

  a + b
end

fn flip(b: Bool) : Bool
  not b
end

test "flip turns true into false"
  assert flip(true) == false
end

fn main()
end

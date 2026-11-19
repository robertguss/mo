module Main

fn small(n: Int) : Bool
  n < 10
end

fn digit(n: Int) : Int
  promise "a digit is small"
    small(result)
    allows n: 9, result: 9
    forbids n: 3, result: 10
  end

  n % 10
end

fn main()
end

module Main

fn same(n: Int) : Int
  promise "stays below a hundred"
    result < 100
    allows n: 1, result: 1
    forbids n: 1, result: 100
  end

  n
end

fn main()
end

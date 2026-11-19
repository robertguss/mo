module Main

fn count_down(n: Int) : Int
  examples
    count_down(3) == 0
  end

  var i = n
  while i != 0
    i = i - 1
  end
  i
end

fn main()
end

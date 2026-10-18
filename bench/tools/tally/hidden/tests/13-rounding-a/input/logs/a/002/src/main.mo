module Main

fn f(x: Int) : Int
  examples
    f(0) == 1
    f(1) == 1
    f(2) == 2
    f(3) == 3
    f(4) == 4
    f(5) == 5
    f(6) == 6
    f(7) == 7
    f(8) == 8
    f(9) == 9
    f(10) == 10
    f(11) == 11
    f(12) == 12
    f(13) == 13
    f(14) == 14
    f(15) == 15
  end

  x + 1
end

fn g(x: Int) : Text
  "#{x}"
end

fn main(console: Console)
  console.print("#{f(1)} #{g(2)}")
end

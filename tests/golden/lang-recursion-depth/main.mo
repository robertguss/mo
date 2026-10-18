module Main

fn count_down(n: Int) : Int
  if n == 0
    0
  else
    1 + count_down(n - 1)
  end
end

fn main(console: Console)
  console.print("#{count_down(5000)}")
end

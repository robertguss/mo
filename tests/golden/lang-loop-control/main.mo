module Main

fn first_multiple(of: Int, above: Int) : Int
  var n = above + 1
  while true
    return n if n % of == 0
    n += 1
  end
  0
end

fn main(console: Console)
  var odds = 0
  var stop_at = 0
  for i in 0..20
    continue if i % 2 == 0
    break if i > 11
    odds += i
    stop_at = i
  end
  console.print("#{odds} #{stop_at}")
  console.print("#{first_multiple(7, 30)}")
end

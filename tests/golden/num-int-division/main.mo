module Main

fn main(console: Console)
  a = -7
  b = 7
  console.print("#{a / 2} #{a % 2}")
  console.print("#{b / -2} #{b % -2}")
  console.print("#{a / -2} #{a % -2}")
  console.print("#{b / 2} #{b % 2}")
  console.print("#{0 / 5} #{0 % 5}")
  console.print("#{100 / 7 * 7 + 100 % 7}")
end

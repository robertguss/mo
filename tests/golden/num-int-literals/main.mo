module Main

fn main(console: Console)
  population = 8_100_000_000
  mask = 0xFF
  flags = 0b1010
  console.print("#{population}")
  console.print("#{mask} #{flags}")
  console.print("#{mask + flags}")
  console.print("#{0xff == 255} #{0b11 == 3}")
  console.print("#{1_000 * 1_000}")
end

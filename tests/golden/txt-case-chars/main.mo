module Main

fn main(console: Console)
  console.print("Hello, World 42".ascii_lowercase)
  console.print("Hello, World 42".ascii_uppercase)
  console.print("éa".ascii_uppercase)
  console.print("#{'A'.code} #{'z'.code} #{'é'.code} #{'0'.code}")
  console.print("#{'0'.ascii_digit?} #{'x'.ascii_digit?} #{'x'.ascii_letter?} #{'é'.ascii_letter?}")
  console.print("#{' '.ascii_white_space?} #{'\t'.ascii_white_space?} #{'a'.ascii_white_space?}")
  console.print("#{'q'.ascii_uppercase}#{'Q'.ascii_lowercase}#{'5'.ascii_uppercase}")
  console.print("#{"a1 b2".chars.count(fn(c) c.ascii_digit? end)}")
end

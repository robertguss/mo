module Main

fn shift(c: Char, by: Int) : Char
  if c.ascii_letter? and c.ascii_lowercase == c
    next = (c.code - 97 + by) % 26 + 97
    next.to_char or c
  else
    c
  end
end

fn encode(text: Text, by: Int) : Text
  Text.from_chars(text.chars.map(fn(c) shift(c, by) end))
end

fn main(console: Console)
  secret = encode("hello, world", 3)
  console.print(secret)
  console.print(encode(secret, 23))
  console.print(encode("xyz abc", 3))
end

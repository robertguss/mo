module Main

fn main(console: Console)
  word = "stressed"
  console.print(Text.from_chars(word.chars.reverse))
  console.print(word.replace("s", ""))
  console.print(Text.from_chars(word.chars.map(fn(c) c.ascii_uppercase end)))
  console.print("#{word.chars.size} #{"héllo".chars}")
  console.print("[#{Text.from_chars("héllo".chars.take(2))}]")
  console.print("#{Text.from_chars(['a', '😀']).byte_count}")
end

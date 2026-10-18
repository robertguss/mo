module Main

fn main(console: Console)
  console.print("#{"a\nb\r\nc\n".lines}")
  console.print("#{"".lines} #{"\n".lines} #{"a\n\nb".lines}")
  console.print("#{"a\rb".lines}")
  console.print("#{"  two   words\t here\n".words}")
  console.print("#{"   ".words} #{"".words}")
  console.print("#{"one".words.size} #{"x y z".words.size}")
end

module Main

fn main(console: Console)
  console.print("[#{"  hi \t\n".trim}] [#{"  hi  ".trim_start}] [#{"  hi  ".trim_end}]")
  console.print("[#{"ab".pad_left(5)}] [#{"ab".pad_right(5)}] [#{"héé".pad_left(5)}]")
  console.print("[#{"abcdef".pad_left(3)}] [#{"abc".pad_left(-2)}] [#{"".pad_right(2)}]")
  console.print("[#{"\u{b}x\u{c}".trim}] [#{"é ".trim_end}]")
end

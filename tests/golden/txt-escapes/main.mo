module Main

fn main(console: Console)
  console.print("tab[\t] quote[\"] backslash[\\] apostrophe[\'] hash[\#]")
  console.print("line1\nline2")
  console.print("smile \u{1F600} lambda \u{3bb}")
  console.print("#{["a\"b", "c\nd"]}")
  console.print("#{['\n', 'a', '\'']}")
  console.print("#{"\u{e9}" == "é"}")
end

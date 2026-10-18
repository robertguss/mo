module Main

fn main(console: Console)
  var n = 0
  var more? = true
  while more?
    case console.read_line
      Some(line):
        n += 1
        console.print("#{n}: [#{line}] #{line.char_count}")
      None:
        more? = false
    end
  end
  console.print("done after #{n}")
end

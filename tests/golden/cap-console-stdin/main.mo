module Main

fn main(console: Console)
  var total = 0
  var lines = 0
  var more? = true
  while more?
    case console.read_line
      Some(line):
        lines += 1
        total += line.trim.to_int or 0
      None:
        more? = false
    end
  end
  console.print("#{lines} lines, sum #{total}")
end

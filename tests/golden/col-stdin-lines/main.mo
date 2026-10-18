module Main

fn main(console: Console)
  var lines = ["x"].drop(1)
  var more = true
  while more
    case console.read_line
      Some(line):
        lines = lines.append(line)
      None:
        more = false
    end
  end
  console.print("#{lines.size} lines")
  console.print("#{lines.reverse}")
end

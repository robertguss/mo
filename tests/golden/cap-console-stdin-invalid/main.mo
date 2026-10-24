module Main

fn main(console: Console)
  while console.read_line is Some(line)
    codes = line.chars.map(fn(c) c.code end)
    console.print("#{line.char_count} #{codes} #{line}")
  end
end

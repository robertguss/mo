module Main

fn main(console: Console)
  bytes = "abcde".bytes
  console.print("#{bytes.slice(1, 3).decode_utf8}")
  console.print(
    "#{bytes.slice(3, 99).decode_utf8} #{bytes.slice(3, 1).size} #{bytes.slice(9, 12).size}"
  )
  console.print("#{Bytes.join([bytes.slice(0, 1), "".bytes, bytes.slice(4, 5)]).decode_utf8}")
  console.print("#{bytes.slice(0, -1).size}")
  console.print("unreachable")
end

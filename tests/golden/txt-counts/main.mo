module Main

fn main(console: Console)
  for word in ["abc", "héllo", "日本語", "😀", ""]
    console.print("#{word}: #{word.byte_count} bytes, #{word.char_count} chars, empty #{word.empty?}")
  end
end

module Main

size = Bytes.size
decode = Bytes.decode_utf8

fn main(console: Console) : Int
  get = Bytes.get
  text = "Aλ日本\u{0}"
  bytes = text.bytes
  console.print("#{bytes.size}/#{size("".bytes)}/#{bytes.get(0)}/#{get(bytes, 1)}/#{get(bytes, 2)}")
  console.print("#{bytes.get(-1)}/#{get(bytes, 10)}/#{"".bytes.get(0)}/#{decode("".bytes)}")
  if size(bytes) == 10 and Bytes.size("λ".bytes) == 2 and
    bytes.get(9) == 0.to_byte and get(bytes, 9223372036854775807) is None and
    bytes.decode_utf8 == Some(text) and decode(bytes) == Some(text) and
    Bytes.decode_utf8("λ".bytes) == Some("λ"): 0 else: 1
end

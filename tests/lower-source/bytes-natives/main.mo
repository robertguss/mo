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
    Bytes.decode_utf8("λ".bytes) == Some("λ") and cut_and_joined(bytes): 0 else: 1
end

fn cut_and_joined(bytes: Bytes) : Bool
  slice = Bytes.slice
  join = Bytes.join
  bytes.slice(0, 1).decode_utf8 == Some("A") and slice(bytes, 1, 3) == "λ".bytes and
    Bytes.slice(bytes, 3, 100) == "日本\u{0}".bytes and bytes.slice(4, 2).size == 0 and
    bytes.slice(11, 12).size == 0 and bytes.slice(0, 9223372036854775807) == bytes and
    Bytes.join([]).size == 0 and join([bytes.slice(0, 1), "".bytes, "λ".bytes]) == "Aλ".bytes and
    Bytes.join([bytes, bytes]).size == 20
end

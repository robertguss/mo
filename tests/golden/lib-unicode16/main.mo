module Main
use Unicode16

fn main(console: Console)
  console.print(Unicode16.lowercase("Ça"))
  console.print(Unicode16.uppercase("Straße"))
  console.print(Unicode16.normalize_nfc("e\u{301}"))
  console.print(Unicode16.graphemes("e\u{301}🇨🇦").to_text)
  console.print(Unicode16.display_width("e\u{301}🇨🇦").to_text)
end

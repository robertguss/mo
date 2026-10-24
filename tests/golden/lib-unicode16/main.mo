module Main
use Unicode16

fn main(console: Console)
  console.print(Unicode16.lowercase("Ça"))
  console.print(Unicode16.uppercase("Straße"))
  console.print(Unicode16.normalize_nfc("e\u{301}"))
  console.print(Unicode16.graphemes("e\u{301}🇨🇦").to_text)
  console.print(Unicode16.display_width("e\u{301}🇨🇦").to_text)
  kept = "éǅʰ界\u{10400}١\u{10D40}²Ⅳ\u{345}".chars.filter(fn(c)
    Unicode16.letter?(c) or Unicode16.digit?(c)
  end)
  console.print(kept.to_text)
end

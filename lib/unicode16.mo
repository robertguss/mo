## Unicode 16.0 operations of reference section 14. The C primitives own the algorithms;
## these tests pin their results through the Mo module.
module Unicode16

test "graphemes splits user-perceived characters without normalizing their bytes"
  assert Unicode16.graphemes("") == []
  assert Unicode16.graphemes("aé!") == ["a", "é", "!"]
  assert Unicode16.graphemes("e\u{301}x") == ["e\u{301}", "x"]
  assert Unicode16.graphemes("a\r\nb") == ["a", "\r\n", "b"]
  assert Unicode16.graphemes("👍🏽👩\u{200D}💻🇨🇦🇺") == ["👍🏽", "👩\u{200D}💻", "🇨🇦", "🇺"]
  assert Unicode16.graphemes("\u{1100}\u{1161}\u{11A8}") == ["\u{1100}\u{1161}\u{11A8}"]
  assert Unicode16.graphemes("क्ष") == ["क्ष"]
end

test "display_width counts columns per grapheme rather than bytes or scalars"
  assert Unicode16.display_width("") == 0
  assert Unicode16.display_width("abc") == 3
  assert Unicode16.display_width("é") == 1
  assert Unicode16.display_width("e\u{301}") == 1
  assert Unicode16.display_width("界Ａ") == 4
  assert Unicode16.display_width("·") == 1
  assert Unicode16.display_width("का") == 1
  assert Unicode16.display_width("क्ष") == 1
  assert Unicode16.display_width("\u{1100}\u{1161}\u{11A8}") == 2
  assert Unicode16.display_width("a\tb\r\nc") == 3
  assert Unicode16.display_width("\u{0}\u{1F}\u{7F}\u{85}\u{301}\u{903}\u{20DD}\u{200D}") == 0
end

test "display_width counts emoji sequences as two columns and lone flag letters as one"
  assert Unicode16.display_width("😀") == 2
  assert Unicode16.display_width("❤") == 1
  assert Unicode16.display_width("❤️") == 2
  assert Unicode16.display_width("👍🏽") == 2
  assert Unicode16.display_width("👩\u{200D}💻") == 2
  assert Unicode16.display_width("1\u{FE0F}\u{20E3}") == 2
  assert Unicode16.display_width("🇨🇦") == 2
  assert Unicode16.display_width("🇨") == 1
  assert Unicode16.display_width("🇨🇦🇺") == 3
  assert Unicode16.display_width("a👩\u{200D}💻界e\u{301}") == 6
end

test "normalize_nfc composes canonical equivalents but not compatibility characters"
  assert Unicode16.normalize_nfc("") == ""
  assert Unicode16.normalize_nfc("plain text") == "plain text"
  assert Unicode16.normalize_nfc("e\u{301}") == "é"
  assert Unicode16.normalize_nfc("é") == "é"
  assert Unicode16.normalize_nfc("a\u{315}\u{300}") == "à\u{315}"
  assert Unicode16.normalize_nfc("\u{1100}\u{1161}\u{11A8}") == "각"
  assert Unicode16.normalize_nfc("ﬁ①") == "ﬁ①"
end

test "lowercase uses full Unicode mappings without locale tailoring or normalization"
  assert Unicode16.lowercase("") == ""
  assert Unicode16.lowercase("Ça") == "ça"
  assert Unicode16.lowercase("ÉCOLE, 123!") == "école, 123!"
  assert Unicode16.lowercase("Iİ") == "ii\u{307}"
  assert Unicode16.lowercase("E\u{301}") == "e\u{301}"
  assert Unicode16.lowercase("\u{10400}") == "\u{10428}"
end

test "lowercase chooses final sigma from the original cased context"
  assert Unicode16.lowercase("Σ") == "σ"
  assert Unicode16.lowercase("ΟΣ") == "ος"
  assert Unicode16.lowercase("ΟΣΑ") == "οσα"
  assert Unicode16.lowercase("Α\u{301}Σ\u{301}") == "α\u{301}ς\u{301}"
  assert Unicode16.lowercase("ΑΣ\u{301}Α") == "ασ\u{301}α"
  assert Unicode16.lowercase("ʰΣ") == "ʰσ"
end

test "uppercase uses full Unicode mappings without locale tailoring or normalization"
  assert Unicode16.uppercase("") == ""
  assert Unicode16.uppercase("Ça") == "ÇA"
  assert Unicode16.uppercase("Straße, 123!") == "STRASSE, 123!"
  assert Unicode16.uppercase("ﬁ") == "FI"
  assert Unicode16.uppercase("iı") == "II"
  assert Unicode16.uppercase("σς") == "ΣΣ"
  assert Unicode16.uppercase("e\u{301}") == "E\u{301}"
  assert Unicode16.uppercase("\u{10428}") == "\u{10400}"
end

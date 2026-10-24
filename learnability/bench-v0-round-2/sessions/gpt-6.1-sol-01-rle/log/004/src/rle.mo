## Run-length encoding of Unicode characters, with ASCII digits reserved for counts.
module Rle
expose encode, decode

fn encode(text: Text) : Option(Text)
  promise "digit-free text encodes successfully and decoding its encoding gives the original text"
    text.chars.any?(fn(c) c.ascii_digit? end) == (result == None)
    result is Some(encoded) implies decode(encoded) == Some(text)
    allows text: "aaabcc", result: Some("3a1b2c")
    allows text: "a1", result: None
    forbids text: "aaabcc", result: Some("2a1b2c")
    forbids text: "a", result: None
  end
  examples
    encode("aaabcc") == Some("3a1b2c")
    encode("ééé") == Some("3é")
    encode("") == Some("")
    encode("a1") == None
  end

  chars = text.chars
  return Some("") if chars.empty?
  var current = try chars.first
  var count = 0
  var runs = []
  for c in chars
    return None if c.ascii_digit?
    if c != current
      runs = runs.append("#{count}#{current}")
      current = c
      count = 0
    end
    count += 1
  end
  runs = runs.append("#{count}#{current}")
  Some(runs.join(""))
end

fn decode(text: Text) : Option(Text)
  examples
    decode("3a1b2c") == Some("aaabcc")
    decode("3é") == Some("ééé")
    decode("2a1a") == Some("aaa")
    decode("") == Some("")
    decode("03a") == None
    decode("0a") == None
    decode("12") == None
    decode("a") == None
    decode("9223372036854775808a") == None
  end

  var count = 0
  var runs = []
  for c in text.chars
    if c.ascii_digit?
      return None if count == 0 and c == '0'
      shifted = try count.checked_mul(10)
      count = try shifted.checked_add(c.code - '0'.code)
    else
      return None if count == 0
      runs = runs.append(c.to_text.repeat(count))
      count = 0
    end
  end
  return None if count != 0
  Some(runs.join(""))
end

test "runs preserve whitespace and Unicode code points"
  assert encode("  \n\n\t") == Some("2 2\n1\t")
  assert decode("2 2\n1\t") == Some("  \n\n\t")
  assert encode("😀😀١١éé") == Some("2😀2١2é")
  assert decode("2😀2١2é") == Some("😀😀١١éé")
  assert encode("e\u{301}e\u{301}") == Some("1e1\u{301}1e1\u{301}")
  assert decode("1e1\u{301}1e1\u{301}") == Some("e\u{301}e\u{301}")
  assert encode("\u{0}\u{0}\r") == Some("2\u{0}1\r")
  assert decode("2\u{0}1\r") == Some("\u{0}\u{0}\r")
end

test "multi-digit counts and adjacent equal runs decode"
  assert encode("aaaaaaaaaaaabba") == Some("12a2b1a")
  assert decode("12a2b1a") == Some("aaaaaaaaaaaabba")
  assert decode("10a2a") == Some("aaaaaaaaaaaa")
end

test "every ASCII digit is rejected wherever it appears in source text"
  for c in "0123456789".chars
    assert encode(c.to_text) == None
    assert encode("#{c}aa") == None
    assert encode("aa#{c}bb") == None
    assert encode("aa#{c}") == None
  end
end

test "malformed runs and overflowing counts are rejected"
  for invalid in [
    "0",
    "00a",
    "1a0b",
    "1a01b",
    "1a2",
    "1ab",
    " 1a",
    "-1a",
    "+1a",
    "١a",
    "9223372036854775807",
    "9223372036854775810a",
    "999999999999999999999999999999a"
  ]
    assert decode(invalid) == None
  end
end

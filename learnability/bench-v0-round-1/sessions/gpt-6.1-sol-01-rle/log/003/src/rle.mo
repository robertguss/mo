## Run-length encoding of Unicode characters, excluding ASCII digits.
module Rle
expose encode, decode

fn encode(text: Text) : Option(Text)
  promise "decoding an encoded text gives the original text back"
    result is Some(encoded) implies decode(encoded) == Some(text)
    allows text: "aaabcc", result: Some("3a1b2c")
    allows text: "", result: Some("")
    allows text: "a1", result: None
    forbids text: "aaabcc", result: Some("2a1b2c")
  end
  examples
    encode("aaabcc") == Some("3a1b2c")
    encode("ééé") == Some("3é")
    encode("") == Some("")
    encode("a1") == None
  end

  var parts = []
  var previous = ' '
  var count = 0
  for character in text.chars
    return None if character.ascii_digit?
    if count > 0 and character != previous
      parts = parts.append("#{count}#{previous}")
      count = 0
    end
    previous = character
    count += 1
  end
  if count > 0
    parts = parts.append("#{count}#{previous}")
  end
  Some(parts.join(""))
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

  var parts = []
  var count = 0
  for character in text.chars
    if character.ascii_digit?
      return None if count == 0 and character == '0'
      scaled = try count.checked_mul(10)
      count = try scaled.checked_add(character.code - '0'.code)
    else
      return None if count == 0
      parts = parts.append(character.to_text.repeat(count))
      count = 0
    end
  end
  return None if count > 0
  Some(parts.join(""))
end

test "runs count Unicode code points and preserve whitespace"
  assert encode("  \n\néé😀😀e\u{301}e\u{301}٢٢") == Some("2 2\n2é2😀1e1\u{301}1e1\u{301}2٢")
  assert decode("2 2\n2é2😀1e1\u{301}1e1\u{301}2٢") == Some("  \n\néé😀😀e\u{301}e\u{301}٢٢")
  assert encode("\u{0}\u{0}\t\r") == Some("2\u{0}1\t1\r")
  assert decode("2\u{0}1\t1\r") == Some("\u{0}\u{0}\t\r")
end

test "multi-digit counts and run boundaries are preserved"
  assert encode("aaaaaaaaaaaabccaaa") == Some("12a1b2c3a")
  assert decode("12a1b2c3a") == Some("aaaaaaaaaaaabccaaa")
  assert decode("10a") == Some("aaaaaaaaaa")
  assert decode("1-1+1.") == Some("-+.")
end

test "ASCII digits are rejected anywhere in the original text"
  for character in "0123456789".chars
    assert encode("#{character}") == None
    assert encode("aa#{character}bb") == None
    assert encode("aa#{character}") == None
  end
end

test "malformed counts and incomplete runs are rejected"
  for invalid in ["0", "00a", "01a", "1a0b", "1a03b", "1a2", "1ab", "-1a", "+1a", " 1a", "٢a"]
    assert decode(invalid) == None
  end
end

test "count overflow is rejected during multiplication and addition"
  assert decode("9223372036854775808a") == None
  assert decode("9223372036854775810a") == None
  assert decode("999999999999999999999999999999999999999999a") == None
  assert decode("1a9223372036854775808b") == None
  assert decode("9223372036854775807") == None
end

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

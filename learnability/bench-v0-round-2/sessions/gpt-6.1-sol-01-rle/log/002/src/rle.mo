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

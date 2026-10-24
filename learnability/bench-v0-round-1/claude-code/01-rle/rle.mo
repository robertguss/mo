## Run-length encoding: each run of one repeated character becomes its length
## in decimal, then the character.
module Rle
expose encode, decode

## Encodes `text` run by run, or gives `None` when it holds an ASCII digit.
fn encode(text: Text) : Option(Text)
  examples
    encode("aaabcc") == Some("3a1b2c")
    encode("ééé") == Some("3é")
    encode("") == Some("")
    encode("a  \n\n") == Some("1a2 2\n")
    encode("ab1") == None
    encode("aaaaaaaaaaaa") == Some("12a")
  end

  promise "decoding what encode gives back returns the text"
    result is Some(encoded) implies decode(encoded) == Some(text)
    allows text: "aab", result: Some("2a1b")
    forbids text: "aab", result: Some("2a")
  end

  var parts = []
  var current = None
  var count = 0
  for c in text.chars
    return None if c.ascii_digit?
    if current == Some(c)
      count += 1
    else
      if current is Some(previous)
        parts = parts.append("#{count}#{previous}")
      end
      current = Some(c)
      count = 1
    end
  end
  if current is Some(last)
    parts = parts.append("#{count}#{last}")
  end
  Some(parts.join(""))
end

## Decodes runs of a count and a character, or gives `None` when `text` isn't
## such a sequence of runs.
fn decode(text: Text) : Option(Text)
  examples
    decode("3a1b2c") == Some("aaabcc")
    decode("3é") == Some("ééé")
    decode("2a1a") == Some("aaa")
    decode("") == Some("")
    decode("12a") == Some("aaaaaaaaaaaa")
    decode("1 2\n") == Some(" \n\n")
    decode("3") == None
    decode("a") == None
    decode("3ab") == None
    decode("0a") == None
    decode("03a") == None
    decode("99999999999999999999a") == None
  end

  var parts = []
  var count = 0
  var digits? = false
  for c in text.chars
    if c.ascii_digit?
      digit = c.code - '0'.code
      return None if not digits? and digit == 0
      count = try count.checked_mul(10)
      count = try count.checked_add(digit)
      digits? = true
    else
      return None if not digits?
      parts = parts.append(c.to_text.repeat(count))
      count = 0
      digits? = false
    end
  end
  return None if digits?
  Some(parts.join(""))
end

test "decode reverses encode on mixed text"
  text = "  héllo,\n\nwörld!!!"
  assert encode(text) is Some(encoded)
  assert decode(encoded) == Some(text)
end

test "a count one past the largest Int gives None"
  assert decode("9223372036854775808a") == None
  assert decode("1a9223372036854775808b") == None
end

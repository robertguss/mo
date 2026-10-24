## Run-length encoding: each run of one repeated character is written as the
## run's length in decimal, then the character.
module Rle
expose encode, decode

## Encodes `text` run by run, or gives `None` when it holds an ASCII digit.
fn encode(text: Text) : Option(Text)
  promise "decoding what encode gives back yields the text, and only texts with digits fail"
    result is Some(encoded) implies decode(encoded) == Some(text)
    (result == None) == text.chars.any?(fn(c) c.ascii_digit? end)
    allows text: "aab", result: Some("2a1b")
    forbids text: "aab", result: Some("2a")
  end

  examples
    encode("aaabcc") == Some("3a1b2c")
    encode("ééé") == Some("3é")
    encode("") == Some("")
    encode("a  \n\nb") == Some("1a2 2\n1b")
    encode("abab") == Some("1a1b1a1b")
    encode("a1") == None
    encode("7") == None
  end

  var pieces = []
  var current = None
  var count = 0
  for c in text.chars
    return None if c.ascii_digit?
    if current == Some(c)
      count += 1
    else
      if current is Some(previous)
        pieces = pieces.append("#{count}#{previous}")
      end
      current = Some(c)
      count = 1
    end
  end
  if current is Some(last)
    pieces = pieces.append("#{count}#{last}")
  end
  Some(pieces.join(""))
end

## Decodes a sequence of runs, each a count without a leading 0 followed by
## one character that isn't an ASCII digit, or gives `None` for anything else.
fn decode(text: Text) : Option(Text)
  examples
    decode("3a1b2c") == Some("aaabcc")
    decode("2a1a") == Some("aaa")
    decode("3é") == Some("ééé")
    decode("") == Some("")
    decode("12x") == Some("xxxxxxxxxxxx")
    decode("2 1\n") == Some("  \n")
    decode("3") == None
    decode("3a2") == None
    decode("a") == None
    decode("2ab") == None
    decode("0a") == None
    decode("03a") == None
    decode("1a03b") == None
    decode("99999999999999999999a") == None
  end

  var pieces = []
  var digits = []
  for c in text.chars
    if c.ascii_digit?
      return None if digits.empty? and c == '0'
      digits = digits.append(c)
    else
      return None if digits.empty?
      count = try Text.from_chars(digits).to_int
      pieces = pieces.append(c.to_text.repeat(count))
      digits = []
    end
  end
  return None if not digits.empty?
  Some(pieces.join(""))
end

test "encoding then decoding gives back a mixed text"
  original = "zzz  yé\n\n\nq"
  assert encode(original) is Some(encoded)
  assert encoded == "3z2 1y1é3\n1q"
  assert decode(encoded) == Some(original)
end

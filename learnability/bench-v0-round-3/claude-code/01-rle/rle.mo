## Run-length encoding: each run of one repeated character is written as the
## run's length in decimal, then the character, so "aaabcc" is "3a1b2c".
module Rle
expose encode, decode

## Encodes `text` run by run, or gives `None` when it holds an ASCII digit,
## which the encoding could not tell apart from a count.
fn encode(text: Text) : Option(Text)
  examples
    encode("aaabcc") == Some("3a1b2c")
    encode("ééé") == Some("3é")
    encode("") == Some("")
    encode("a") == Some("1a")
    encode("aabaa") == Some("2a1b2a")
    encode("  \n\n\n") == Some("2 3\n")
    encode("a".repeat(12)) == Some("12a")
    encode("abc1") == None
    encode("7") == None
  end

  promise "decoding what encode gives returns the text, and only digits are refused"
    result is Some(encoded) implies decode(encoded) == Some(text)
    (result == None) == text.chars.any?(fn(c) c.ascii_digit? end)
    allows text: "aab", result: Some("2a1b")
    forbids text: "aab", result: Some("2a2b")
    forbids text: "a1", result: Some("1a11")
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

## Decodes a sequence of runs, each one or more ASCII digits without a leading
## 0 followed by one character that isn't an ASCII digit. Anything else gives
## `None`.
fn decode(text: Text) : Option(Text)
  examples
    decode("3a1b2c") == Some("aaabcc")
    decode("3é") == Some("ééé")
    decode("") == Some("")
    decode("2a1a") == Some("aaa")
    decode("12x") == Some("xxxxxxxxxxxx")
    decode("2 3\n") == Some("  \n\n\n")
    decode("3") == None
    decode("3a2") == None
    decode("a") == None
    decode("3ab") == None
    decode("0a") == None
    decode("03a") == None
    decode("1a0b") == None
    decode("9223372036854775808a") == None
    decode("99999999999999999999a") == None
  end

  var parts = []
  var count = None
  for c in text.chars
    if c.ascii_digit?
      digit = c.code - '0'.code
      count = case count
        None if digit == 0: return None
        None: Some(digit)
        Some(n):
          tens = try n.checked_mul(10)
          Some(try tens.checked_add(digit))
      end
    else
      n = try count
      parts = parts.append(c.to_text.repeat(n))
      count = None
    end
  end
  return None if count != None
  Some(parts.join(""))
end

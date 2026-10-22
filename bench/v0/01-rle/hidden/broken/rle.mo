## Run-length encoding: each run of one repeated character is written as its
## length, then the character.
module Rle
expose encode, decode

## The run-length form of `text`, or `None` when it holds an ASCII digit.
fn encode(text: Text) : Option(Text)
  promise "decoding an encoded text gives the text back"
    result is Some(encoded) implies decode(encoded) == Some(text)
    allows text: "aab", result: Some("2a1b")
    allows text: "a1", result: None
    forbids text: "aab", result: Some("1a2b")
  end
  examples
    encode("") == Some("")
    encode("aaabcc") == Some("3a1b2c")
    encode("abc") == Some("1a1b1c")
    encode("ééé  ") == Some("3é2 ")
    encode("route 66") == None
  end

  return None if text.chars.any?(fn(c) c.ascii_digit? end)
  var pieces = []
  var current = None
  var length = 0
  for c in text.chars
    if current == Some(c)
      length += 1
    else
      if current is Some(previous)
        pieces = pieces.append("#{length}#{previous}")
      end
      current = Some(c)
      length = 1
    end
  end
  if current is Some(last)
    pieces = pieces.append("#{length}#{last}")
  end
  Some(pieces.join(""))
end

## The text whose runs `text` lists, or `None` when `text` isn't a list of runs.
fn decode(text: Text) : Option(Text)
  examples
    decode("") == Some("")
    decode("3a1b2c") == Some("aaabcc")
    decode("2a1a") == Some("aaa")
    decode("12x") == Some("xxxxxxxxxxxx")
    decode("3") == None
    decode("a") == None
  end

  var pieces = []
  var digits = ""
  for c in text.chars
    if c.ascii_digit?
      digits = "#{digits}#{c}"
    else
      count = try digits.to_int
      pieces = pieces.append(c.to_text.repeat(count))
      digits = ""
    end
  end
  return None if not digits.empty?
  Some(pieces.join(""))
end

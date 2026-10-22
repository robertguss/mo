## The hidden tests of task 01-rle.
module HiddenTests
use Rle

test "an empty text has no runs"
  assert Rle.encode("") == Some("")
  assert Rle.decode("") == Some("")
end

test "runs are counted"
  assert Rle.encode("aaabcc") == Some("3a1b2c")
  assert Rle.encode("abc") == Some("1a1b1c")
  assert Rle.encode("zzzzzzzzzzzz") == Some("12z")
end

test "a run's character can be any character but a digit"
  assert Rle.encode("  ..éé!") == Some("2 2.2é1!")
  assert Rle.encode("a\nb") == Some("1a1\n1b")
end

test "a text with a digit can't be encoded"
  assert Rle.encode("route 66") == None
  assert Rle.encode("0") == None
end

test "runs are read back"
  assert Rle.decode("3a1b2c") == Some("aaabcc")
  assert Rle.decode("12z") == Some("zzzzzzzzzzzz")
  assert Rle.decode("2 2.2é1!") == Some("  ..éé!")
end

test "runs of one character need not be joined"
  assert Rle.decode("2a1a") == Some("aaa")
end

test "a count without a character, or a character without a count, isn't a run"
  assert Rle.decode("3") == None
  assert Rle.decode("3a2") == None
  assert Rle.decode("a") == None
  assert Rle.decode("2ab") == None
end

test "a count is at least 1 and has no leading zero"
  assert Rle.decode("0a") == None
  assert Rle.decode("03a") == None
  assert Rle.decode("10a") == Some("aaaaaaaaaa")
end

test "a count too large for an Int isn't a run"
  assert Rle.decode("99999999999999999999a") == None
end

test "encoding then decoding gives the text back"
  for text in ["", "a", "aab", "xyzzy", "  tabs\t\there  "]
    assert Rle.encode(text) is Some(encoded)
    assert Rle.decode(encoded) == Some(text)
  end
end

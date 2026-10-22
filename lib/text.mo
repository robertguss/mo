## Text's operations of reference section 14. Each is a primitive written in C; these tests pin
## what the reference says they do.
module Text

test "byte_count counts UTF-8 bytes and char_count counts characters"
  assert "".byte_count == 0
  assert "".char_count == 0
  assert "abc".byte_count == 3
  assert "abc".char_count == 3
  assert "é".byte_count == 2
  assert "é".char_count == 1
  assert "\u{1F600}".byte_count == 4
  assert "a\u{1F600}b".char_count == 3
end

test "empty? is true only for the empty text"
  assert "".empty?
  assert not " ".empty?
  assert not "a".empty?
end

test "chars gives every character in order, and from_chars puts them back"
  assert "".chars == []
  assert "aé\u{1F600}".chars == ['a', 'é', '\u{1F600}']
  assert "a\nb".chars == ['a', '\n', 'b']
  assert Text.from_chars([]) == ""
  assert Text.from_chars(['h', 'é', '\u{1F600}']) == "hé\u{1F600}"
  assert Text.from_chars("any text, é".chars) == "any text, é"
end

test "bytes gives the UTF-8 bytes"
  assert "".bytes == "".bytes
  assert "é".bytes == "é".bytes
  assert "é".bytes != "e".bytes
  assert "é".bytes != "e\u{301}".bytes
end

test "lines splits at \n and \r\n, without the line endings"
  assert "".lines == []
  assert "a".lines == ["a"]
  assert "a\nb".lines == ["a", "b"]
  assert "a\r\nb\r\n".lines == ["a", "b"]
  assert "a\n".lines == ["a"]
  assert "a\n\nb".lines == ["a", "", "b"]
  assert "\n".lines == [""]
  assert "a\rb".lines == ["a\rb"]
  assert "a\r".lines == ["a\r"]
end

test "words splits at ASCII white space"
  assert "".words == []
  assert " \t\n\u{0B}\u{0C}\r".words == []
  assert "one two".words == ["one", "two"]
  assert "  one \t two\r\nthree  ".words == ["one", "two", "three"]
  assert "a\u{A0}b".words == ["a\u{A0}b"]
end

test "split keeps empty pieces"
  assert "".split(",") == [""]
  assert "a,b,c".split(",") == ["a", "b", "c"]
  assert ",a,,b,".split(",") == ["", "a", "", "b", ""]
  assert "a, b".split(", ") == ["a", "b"]
  assert "abc".split("x") == ["abc"]
  assert "aaa".split("aa") == ["", "a"]
  assert "aéb".split("é") == ["a", "b"]
end

test "trim, trim_start and trim_end drop ASCII white space"
  assert " \t a b \r\n".trim == "a b"
  assert " \t a b \r\n".trim_start == "a b \r\n"
  assert " \t a b \r\n".trim_end == " \t a b"
  assert "   ".trim == ""
  assert "".trim == ""
  assert "\u{A0}a\u{A0}".trim == "\u{A0}a\u{A0}"
  assert "\u{0B}\u{0C}a".trim_start == "a"
end

test "starts_with?, ends_with? and contains? look for a part"
  assert "report.md".starts_with?("report")
  assert not "report.md".starts_with?("md")
  assert "report.md".ends_with?(".md")
  assert not "report.md".ends_with?("report")
  assert "report.md".contains?("t.m")
  assert not "report.md".contains?("x")
  assert "abc".starts_with?("")
  assert "abc".ends_with?("")
  assert "abc".contains?("")
  assert "".contains?("")
  assert not "ab".starts_with?("abc")
  assert not "ab".ends_with?("abc")
end

test "drop_prefix and drop_suffix give None when the text doesn't have it"
  assert "v1.2".drop_prefix("v") == Some("1.2")
  assert "v1.2".drop_prefix("x") == None
  assert "notes.md".drop_suffix(".md") == Some("notes")
  assert "notes.md".drop_suffix(".txt") == None
  assert "ab".drop_prefix("ab") == Some("")
  assert "ab".drop_suffix("") == Some("ab")
  assert "a".drop_prefix("ab") == None
end

test "replace replaces every occurrence, from the left"
  assert "a-b-c".replace("-", "+") == "a+b+c"
  assert "aaa".replace("aa", "b") == "ba"
  assert "abc".replace("x", "y") == "abc"
  assert "abc".replace("b", "") == "ac"
  assert "".replace("a", "b") == ""
  assert "é".replace("é", "e") == "e"
end

test "repeat joins n copies"
  assert "ab".repeat(3) == "ababab"
  assert "ab".repeat(1) == "ab"
  assert "ab".repeat(0) == ""
  assert "".repeat(5) == ""
end

test "pad_left and pad_right add spaces up to a width in characters"
  assert "7".pad_left(3) == "  7"
  assert "7".pad_right(3) == "7  "
  assert "é".pad_left(3) == "  é"
  assert "é".pad_right(2) == "é "
  assert "abc".pad_left(3) == "abc"
  assert "abcd".pad_right(3) == "abcd"
  assert "ab".pad_left(-1) == "ab"
  assert "".pad_right(2) == "  "
end

test "ascii_lowercase and ascii_uppercase change only ASCII letters"
  assert "Hello, World 1!".ascii_lowercase == "hello, world 1!"
  assert "Hello, World 1!".ascii_uppercase == "HELLO, WORLD 1!"
  assert "ÉTé".ascii_lowercase == "Été"
  assert "ÉTé".ascii_uppercase == "ÉTé"
  assert "".ascii_uppercase == ""
end

test "to_int reads an optional - and ASCII digits in Int's range, and nothing else"
  assert "0".to_int == Some(0)
  assert "42".to_int == Some(42)
  assert "-42".to_int == Some(-42)
  assert "007".to_int == Some(7)
  assert "9223372036854775807".to_int == Some(9223372036854775807)
  assert "-9223372036854775808".to_int == Some(-9223372036854775807 - 1)
  assert "9223372036854775808".to_int == None
  assert "-9223372036854775809".to_int == None
  assert "".to_int == None
  assert "-".to_int == None
  assert "+1".to_int == None
  assert " 1".to_int == None
  assert "1 ".to_int == None
  assert "1.0".to_int == None
  assert "1_000".to_int == None
  assert "١".to_int == None
end

test "to_decimal reads digits and up to 18 places, and nothing else"
  assert "1".to_decimal == Some(1.0)
  assert "-2.50".to_decimal == Some(-2.5)
  assert "0.000000000000000001".to_decimal == Some(0.000000000000000001)
  assert "0.0000000000000000001".to_decimal == None
  assert "1.".to_decimal == None
  assert ".5".to_decimal == None
  assert "+1".to_decimal == None
  assert "1e2".to_decimal == None
  assert "".to_decimal == None
  assert " 1".to_decimal == None
end

test "to_float reads a number with an optional exponent, or nan, inf and -inf"
  assert "1".to_float == Some(1.0)
  assert "-2.5".to_float == Some(-2.5)
  assert "1e3".to_float == Some(1000.0)
  assert "1.5E-1".to_float == Some(0.15)
  assert "2e+2".to_float == Some(200.0)
  assert "0.1".to_float == Some(0.1)
  assert "inf".to_float.map(fn(f) f.to_text end) == Some("inf")
  assert "-inf".to_float.map(fn(f) f.to_text end) == Some("-inf")
  assert "nan".to_float.map(fn(f) f.to_text end) == Some("nan")
  assert "NaN".to_float == None
  assert "Infinity".to_float == None
  assert "1e400".to_float == None
  assert "1e-400".to_float == Some(0.0)
  assert "1.".to_float == None
  assert ".5".to_float == None
  assert "1e".to_float == None
  assert "+1".to_float == None
  assert "".to_float == None
end

test "texts are equal by their bytes and sort by character code"
  assert "abc" == "abc"
  assert "é" != "e\u{301}"
  assert "a" < "b"
  assert "Z" < "a"
  assert "ab" < "abc"
  assert "" < "a"
  assert "z" < "é"
  assert "a".compare("b") == Less
  assert "b".compare("b") == Equal
  assert "é".compare("z") == Greater
  assert ["b", "", "é", "B", "ab"].sort == ["", "B", "ab", "b", "é"]
end

test "a text's text form is the text itself"
  assert "a \"b\"".to_text == "a \"b\""
  assert "#{"x"}" == "x"
end

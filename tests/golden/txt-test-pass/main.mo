module Main

fn initials(name: Text) : Text
  examples
    initials("ada lovelace") == "AL"
    initials("  grace   hopper ") == "GH"
    initials("") == ""
  end

  Text.from_chars(name.words.map(fn(w) (w.chars.first or ' ').ascii_uppercase end))
end

fn slug(title: Text) : Text
  examples
    slug("Hello World") == "hello-world"
  end

  title.words.join("-").ascii_lowercase
end

fn main(console: Console)
  console.print(slug("Mo Language"))
end

test "initials of a single word"
  assert initials("plato") == "P"
end

test "slugs are lowercase"
  assert slug("A B C") == "a-b-c"
  assert slug("X") == "x"
end

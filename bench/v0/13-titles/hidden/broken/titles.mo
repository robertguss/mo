## Writes titles in title case, as an English style guide does.
module Titles
expose capitalize, title_case

small_words = [
  "a",
  "an",
  "and",
  "as",
  "at",
  "but",
  "by",
  "for",
  "in",
  "of",
  "on",
  "or",
  "the",
  "to"
]

## The word with its first character in upper case and the rest in lower case, in ASCII.
fn capitalize(word: Text) : Text
  examples
    capitalize("hELLO") == "Hello"
    capitalize("") == ""
    capitalize("élan") == "élan"
  end

  case word.chars
    []: ""
    [first, ..rest]: "#{first.ascii_uppercase}#{Text.from_chars(rest).ascii_lowercase}"
  end
end

## Each part of a hyphenated word capitalized.
fn capitalize_parts(word: Text) : Text
  word.split("-").map(capitalize).join("-")
end

## `text` in title case: words capitalized, except small words inside the title.
fn title_case(text: Text) : Text
  examples
    title_case("the lord of the rings") == "The Lord of the Rings"
    title_case("  war   and peace ") == "War and Peace"
    title_case("") == ""
  end

  words = text.words
  last = words.size - 1
  words.enumerate.map(fn(pair)
    (index, word) = pair
    lower = word.ascii_lowercase
    return capitalize_parts(word) if index == 0 and (lower == "the" or lower == "of")
    if index < last and small_words.contains?(lower): lower else: capitalize_parts(word)
  end).join(" ")
end

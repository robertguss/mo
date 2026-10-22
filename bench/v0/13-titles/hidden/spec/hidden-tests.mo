## The hidden tests of task 13-titles.
module HiddenTests
use Titles

test "any small word that starts a title is capitalized"
  assert Titles.title_case("a farewell to arms") == "A Farewell to Arms"
  assert Titles.title_case("and then there were none") == "And Then There Were None"
  assert Titles.title_case("to kill a mockingbird") == "To Kill a Mockingbird"
  assert Titles.title_case("THE OLD MAN AND THE SEA") == "The Old Man and the Sea"
end

test "a hyphenated word that starts a title is capitalized"
  assert Titles.title_case("the-end of it") == "The-End of It"
end

test "a title of one word is capitalized"
  assert Titles.title_case("the") == "The"
  assert Titles.title_case("of") == "Of"
  assert Titles.title_case("dune") == "Dune"
end

test "a title of two small words capitalizes both"
  assert Titles.title_case("of the") == "Of The"
end

test "small words inside stay small, wherever they are"
  assert Titles.title_case("the call of the wild") == "The Call of the Wild"
  assert Titles.title_case("the lion, the witch and the wardrobe") ==
    "The Lion, the Witch and the Wardrobe"
end

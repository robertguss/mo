## Tests of the Titles module.
module TitlesTest
use Titles

test "a word is capitalized in ASCII"
  assert Titles.capitalize("hello") == "Hello"
  assert Titles.capitalize("hELLO") == "Hello"
  assert Titles.capitalize("x") == "X"
  assert Titles.capitalize("") == ""
  assert Titles.capitalize("élan") == "élan"
end

test "every word is capitalized but the small ones"
  assert Titles.title_case("war and peace") == "War and Peace"
  assert Titles.title_case("crime and punishment") == "Crime and Punishment"
  assert Titles.title_case("PRIDE AND PREJUDICE") == "Pride and Prejudice"
end

test "the first word is always capitalized"
  assert Titles.title_case("the lord of the rings") == "The Lord of the Rings"
  assert Titles.title_case("of mice and men") == "Of Mice and Men"
end

test "the last word is always capitalized"
  assert Titles.title_case("what dreams are made of") == "What Dreams Are Made Of"
end

test "spaces are collapsed"
  assert Titles.title_case("  much   ado about nothing ") == "Much Ado About Nothing"
  assert Titles.title_case("") == ""
  assert Titles.title_case("   ") == ""
end

test "each part of a hyphenated word is capitalized"
  assert Titles.title_case("my well-known story") == "My Well-Known Story"
end

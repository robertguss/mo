## Roman numerals: whole numbers from 1 to 3999 written as standard Roman
## numerals, and standard numerals read back as numbers.
module Roman
expose to_roman, from_roman

## The symbols and the subtractive pairs, each with its value, largest first.
symbols = [
  (1000, "M"), (900, "CM"), (500, "D"), (400, "CD"),
  (100, "C"), (90, "XC"), (50, "L"), (40, "XL"),
  (10, "X"), (9, "IX"), (5, "V"), (4, "IV"),
  (1, "I")
]

## The standard numeral for `n`, or `None` when `n` is outside 1 to 3999.
fn to_roman(n: Int) : Option(Text)
  promise "turning a number into a numeral and reading it back gives the number"
    result is Some(text) implies from_roman(text) == Some(n)
    allows n: 14, result: Some("XIV")
    allows n: 0, result: None
    forbids n: 14, result: Some("XVI")
    forbids n: 4, result: Some("IIII")
  end
  promise "exactly the numbers from 1 to 3999 have a numeral"
    (result != None) == (n >= 1 and n <= 3999)
    allows n: 3999, result: Some("MMMCMXCIX")
    allows n: 4000, result: None
    forbids n: 4000, result: Some("MMMM")
    forbids n: 1, result: None
  end
  examples
    to_roman(1) == Some("I")
    to_roman(3) == Some("III")
    to_roman(4) == Some("IV")
    to_roman(9) == Some("IX")
    to_roman(14) == Some("XIV")
    to_roman(40) == Some("XL")
    to_roman(90) == Some("XC")
    to_roman(400) == Some("CD")
    to_roman(900) == Some("CM")
    to_roman(1994) == Some("MCMXCIV")
    to_roman(2026) == Some("MMXXVI")
    to_roman(3888) == Some("MMMDCCCLXXXVIII")
    to_roman(3999) == Some("MMMCMXCIX")
    to_roman(0) == None
    to_roman(-7) == None
    to_roman(4000) == None
  end

  return None if n < 1 or n > 3999
  var rest = n
  var numeral = ""
  for (value, symbol) in symbols
    while rest >= value
      numeral = "#{numeral}#{symbol}"
      rest -= value
    end
  end
  Some(numeral)
end

## The number a standard numeral stands for, or `None` for any other text.
fn from_roman(text: Text) : Option(Int)
  promise "a numeral is read only when it is the standard way to write its number"
    result is Some(number) implies to_roman(number) == Some(text)
    allows text: "IV", result: Some(4)
    allows text: "IIII", result: None
    forbids text: "IIII", result: Some(4)
    forbids text: "IV", result: Some(6)
  end
  examples
    from_roman("I") == Some(1)
    from_roman("III") == Some(3)
    from_roman("IV") == Some(4)
    from_roman("IX") == Some(9)
    from_roman("XIV") == Some(14)
    from_roman("XLII") == Some(42)
    from_roman("XCIX") == Some(99)
    from_roman("CDXLIV") == Some(444)
    from_roman("MCMXCIV") == Some(1994)
    from_roman("MMXXVI") == Some(2026)
    from_roman("MMMDCCCLXXXVIII") == Some(3888)
    from_roman("MMMCMXCIX") == Some(3999)
    from_roman("IIII") == None
    from_roman("VV") == None
    from_roman("IIV") == None
    from_roman("IC") == None
    from_roman("XM") == None
    from_roman("MMMM") == None
    from_roman("iv") == None
    from_roman(" IV") == None
    from_roman("12") == None
    from_roman("") == None
  end

  # the longest standard numeral is MMMDCCCLXXXVIII, 15 letters, and a text
  # no longer than that can't overflow the total
  return None if text.byte_count > 15
  var total = 0
  var largest = 0
  for symbol in text.chars.reverse
    value = try symbol_value(symbol)
    # read from the right: a symbol smaller than one to its right is taken
    # away, like the I in IX
    if value < largest
      total -= value
    else
      total += value
      largest = value
    end
  end
  # that reading is lenient, IIII gives 4 too, so the text counts only when
  # it is exactly the standard numeral for the total
  if to_roman(total) == Some(text): Some(total) else: None
end

## The value of one Roman symbol, or `None` for any other character.
fn symbol_value(symbol: Char) : Option(Int)
  examples
    symbol_value('I') == Some(1)
    symbol_value('M') == Some(1000)
    symbol_value('i') == None
    symbol_value('7') == None
  end

  case symbol
    'I': Some(1)
    'V': Some(5)
    'X': Some(10)
    'L': Some(50)
    'C': Some(100)
    'D': Some(500)
    'M': Some(1000)
    _: None
  end
end

## A number converted to its numeral, or a numeral to its number.
fn convert(arg: Text) : Option(Text)
  examples
    convert("1994") == Some("MCMXCIV")
    convert("MCMXCIV") == Some("1994")
    convert("0") == None
    convert("-4") == None
    convert("4000") == None
    convert("IIII") == None
    convert("ten") == None
    convert("") == None
  end

  case arg.to_int
    Some(number): to_roman(number)
    None: from_roman(arg).map(fn(value) value.to_text end)
  end
end

## Converts each argument, a number to a numeral or a numeral to a number,
## printing ? for one it can't convert. Exits with 1 if any couldn't be.
fn main(args: Args, console: Console) : Int
  var status = 0
  for arg in args.list
    converted = convert(arg)
    console.print(converted or "?")
    if converted == None
      status = 1
    end
  end
  status
end

test "every number from 1 to 3999 comes back from its numeral"
  for number in 1..4000
    assert to_roman(number) is Some(numeral)
    assert from_roman(numeral) == Some(number)
  end
end

test "main prints a numeral for each number and a number for each numeral"
  console = Console.fake()
  assert main(Args.of(["1994", "MMXXVI", "4"]), console) == 0
  assert console.printed == ["MCMXCIV", "2026", "IV"]
end

test "main prints ? for each argument it can't convert, and exits with 1"
  console = Console.fake()
  assert main(Args.of(["IIII", "XIV", "4000", "seven"]), console) == 1
  assert console.printed == ["?", "14", "?", "?"]
end

test "main with no arguments prints nothing and exits with 0"
  console = Console.fake()
  assert main(Args.of([]), console) == 0
  assert console.printed == []
end

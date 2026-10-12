## Converts numbers from 1 to 3999 and standard uppercase Roman numerals.
module Roman
expose to_roman, from_roman

numerals = [
  (1000, "M"), (900, "CM"), (500, "D"), (400, "CD"),
  (100, "C"), (90, "XC"), (50, "L"), (40, "XL"),
  (10, "X"), (9, "IX"), (5, "V"), (4, "IV"), (1, "I")
]

fn to_roman(n: Int) : Option(Text)
  promise "exactly numbers from 1 to 3999 have numerals, which convert back to the original number"
    (n >= 1 and n <= 3999) == (result is Some(_))
    result is Some(numeral) implies from_roman(numeral) == Some(n)
    allows n: 1, result: Some("I")
    allows n: 3999, result: Some("MMMCMXCIX")
    allows n: 0, result: None
    forbids n: 4, result: Some("IIII")
    forbids n: 4, result: None
    forbids n: 4000, result: Some("MMMM")
  end
  examples
    to_roman(1) == Some("I")
    to_roman(4) == Some("IV")
    to_roman(9) == Some("IX")
    to_roman(49) == Some("XLIX")
    to_roman(944) == Some("CMXLIV")
    to_roman(1984) == Some("MCMLXXXIV")
    to_roman(3999) == Some("MMMCMXCIX")
    to_roman(0) == None
    to_roman(-1) == None
    to_roman(4000) == None
  end

  return None if n < 1 or n > 3999
  var remaining = n
  var output = ""
  for (value, symbol) in numerals
    while remaining >= value
      output = "#{output}#{symbol}"
      remaining -= value
    end
  end
  Some(output)
end

fn from_roman(text: Text) : Option(Int)
  examples
    from_roman("I") == Some(1)
    from_roman("IV") == Some(4)
    from_roman("IX") == Some(9)
    from_roman("XLIX") == Some(49)
    from_roman("CMXLIV") == Some(944)
    from_roman("MCMLXXXIV") == Some(1984)
    from_roman("MMMCMXCIX") == Some(3999)
    from_roman("") == None
    from_roman("IIII") == None
    from_roman("IL") == None
    from_roman("IIV") == None
    from_roman("IXIX") == None
    from_roman("MMMM") == None
    from_roman("iv") == None
    from_roman(" IV ") == None
    from_roman("4") == None
  end

  return None if text.empty?
  var remaining = text
  var total = 0
  for (value, symbol) in numerals
    while remaining.drop_prefix(symbol) is Some(rest)
      remaining = rest
      total += value
      return None if total > 3999
    end
  end
  return None if not remaining.empty?
  canonical = try to_roman(total)
  if canonical == text: Some(total) else: None
end

fn main(args: Args, console: Console) : Int
  var status = 0
  for argument in args.list
    converted = case argument.to_int
      Some(n): to_roman(n)
      None: from_roman(argument).map(fn(n) n.to_text end)
    end
    case converted
      Some(output): console.print(output)
      None:
        console.print("?")
        status = 1
    end
  end
  status
end

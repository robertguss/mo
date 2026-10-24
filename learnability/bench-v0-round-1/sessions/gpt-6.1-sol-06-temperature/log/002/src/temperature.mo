## Converts temperatures between Celsius and Fahrenheit.
module Temperature
expose Scale(..), Reading(..), parse, convert, describe, main

enum Scale
  Celsius
  Fahrenheit
end

struct Reading
  degrees: Decimal
  scale: Scale
end

fn symbol(scale: Scale) : Text
  case scale
    Celsius: "C"
    Fahrenheit: "F"
  end
end

## Reads a temperature such as `21.5C` or `-40f`.
fn parse(text: Text) : Option(Reading)
  examples
    parse("21.5C") == Some(Reading(degrees: 21.5, scale: Celsius))
    parse("-40f") == Some(Reading(degrees: -40, scale: Fahrenheit))
    parse("21.5") == None
    parse("C") == None
  end

  trimmed = text.trim
  scale = case trimmed.ascii_uppercase.chars.last
    Some('C'): Celsius
    Some('F'): Fahrenheit
    Some(_) | None: return None
  end
  number = trimmed.chars.take(trimmed.char_count - 1)
  degrees = try Text.from_chars(number).to_decimal
  Some(Reading(degrees: degrees, scale: scale))
end

## The same temperature in `scale`, rounded to one decimal place.
fn convert(reading: Reading, scale: Scale) : Reading
  examples
    convert(Reading(degrees: 100, scale: Celsius), Fahrenheit) ==
      Reading(degrees: 212, scale: Fahrenheit)
    convert(Reading(degrees: 70, scale: Fahrenheit), Celsius) ==
      Reading(degrees: 21.1, scale: Celsius)
    convert(Reading(degrees: 5, scale: Celsius), Celsius) == Reading(degrees: 5, scale: Celsius)
  end

  degrees = case (reading.scale, scale)
    (Celsius, Fahrenheit): reading.degrees * 9 / 5 + 32
    (Fahrenheit, Celsius): (reading.degrees - 32) * 5 / 9
    (Celsius, Celsius) | (Fahrenheit, Fahrenheit): reading.degrees
  end
  Reading(degrees: degrees.round(1), scale: scale)
end

## A line such as `21.5C = 70.7F`, converting to the other scale.
fn describe(reading: Reading) : Text
  examples
    describe(Reading(degrees: 21.5, scale: Celsius)) == "21.5C = 70.7F"
    describe(Reading(degrees: -40, scale: Fahrenheit)) == "-40F = -40C"
  end

  other = case reading.scale
    Celsius: Fahrenheit
    Fahrenheit: Celsius
  end
  converted = convert(reading, other)
  reading.degrees.to_text() + symbol(reading.scale) + " = " + converted.degrees.to_text() + symbol(other)
end

fn main(args: Args, console: Console) : Int
  status = 0
  for text in args.list
    case parse(text)
      Some(reading): console.print(describe(reading))
      None:
        console.error("not a temperature: #{text}")
        status = 1
    end
  end
  status
end

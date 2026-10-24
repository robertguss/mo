## Tests of the Temperature module.
module TemperatureTest
use Temperature{Reading, Scale}

test "readings are parsed in either scale and either case"
  assert Temperature.parse("21.5C") == Some(Reading(degrees: 21.5, scale: Celsius))
  assert Temperature.parse("70F") == Some(Reading(degrees: 70, scale: Fahrenheit))
  assert Temperature.parse("-3c") == Some(Reading(degrees: -3, scale: Celsius))
  assert Temperature.parse(" 0f ") == Some(Reading(degrees: 0, scale: Fahrenheit))
end

test "a reading needs a number and a scale"
  assert Temperature.parse("21.5") == None
  assert Temperature.parse("C") == None
  assert Temperature.parse("21.5K") == None
  assert Temperature.parse("hot") == None
  assert Temperature.parse("") == None
end

test "conversions are rounded to one decimal place"
  boiling = Reading(degrees: 100, scale: Celsius)
  assert Temperature.convert(boiling, Fahrenheit) == Reading(degrees: 212, scale: Fahrenheit)
  room = Reading(degrees: 70, scale: Fahrenheit)
  assert Temperature.convert(room, Celsius) == Reading(degrees: 21.1, scale: Celsius)
  body = Reading(degrees: 98.6, scale: Fahrenheit)
  assert Temperature.convert(body, Celsius) == Reading(degrees: 37, scale: Celsius)
end

test "converting to the same scale only rounds"
  reading = Reading(degrees: 12.25, scale: Celsius)
  assert Temperature.convert(reading, Celsius) == Reading(degrees: 12.3, scale: Celsius)
end

test "a description converts to the other scale"
  assert Temperature.describe(Reading(degrees: 21.5, scale: Celsius)) == "21.5C = 70.7F"
  assert Temperature.describe(Reading(degrees: -40, scale: Fahrenheit)) == "-40F = -40C"
end

test "main describes each argument and reports the rest"
  console = Console.fake()
  assert Temperature.main(Args.of(["100C", "warm", "32F"]), console) == 1
  assert console.printed == ["100C = 212F", "32F = 0C"]
  assert console.printed_errors == ["not a temperature: warm"]
end

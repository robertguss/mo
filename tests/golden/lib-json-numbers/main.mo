module Main
use Json

fn number(text: Text) : Json.JsonNumber
  if Json.parse(text) is Ok(Json.Number(n))
    n
  else
    stop "a JSON number"
  end
end

fn main(console: Console)
  console.print("#{number("1") == number("1")} #{number("1") == number("1.0")}")
  console.print("#{number("1e2") == number("1E2")} #{number("-0") == number("0")}")
  console.print("#{Json.parse("[1.50]") == Json.parse("[1.5]")} #{Json.parse("15") == Ok(Json.int(15))}")
  for text in ["9223372036854775807", "9223372036854775808", "-9223372036854775808", "-9223372036854775809", "1.5", "170000000000000000000", "180000000000000000000", "1e400", "-1e400"]
    n = number(text)
    console.print("#{n.to_text}: #{n.to_int} #{n.to_decimal} #{n.to_float}")
  end
  console.print("#{Json.parse("[1E+2, -0.50, 1e-7]").map(fn(j) j.to_text end)}")
  console.print("#{Json.decimal(2.50).to_text} #{Json.int(-3).to_text}")
end

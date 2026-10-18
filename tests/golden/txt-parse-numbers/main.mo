module Main

fn main(console: Console)
  for text in ["42", "-17", "+5", " 5", "5x", "", "9223372036854775807", "9223372036854775808"]
    console.print("int [#{text}] #{text.to_int}")
  end
  for text in ["3.14", "1.", ".5", "-0.5", "7", "1.234567890123456789", "1.2345678901234567890"]
    console.print("decimal [#{text}] #{text.to_decimal}")
  end
  for text in ["1.5", "1e3", "-2.5E-1", "nan", "inf", "-inf", "1e999", "abc", ".5"]
    console.print("float [#{text}] #{text.to_float}")
  end
end

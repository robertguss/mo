module Main

enum QuantityError
  Empty
  NotANumber(text: Text)
  Negative(value: Int)
end

fn parse_quantity(text: Text) : Result(Int, QuantityError)
  trimmed = text.trim
  return Error(Empty) if trimmed.empty?
  case trimmed.to_int
    Some(n) if n < 0: Error(Negative(value: n))
    Some(n): Ok(n)
    None: Error(NotANumber(text: trimmed))
  end
end

fn describe(text: Text) : Text
  case parse_quantity(text)
    Ok(n): "ok #{n}"
    Error(Empty): "empty"
    Error(NotANumber(text: t)): "not a number: #{t}"
    Error(Negative(value: v)): "negative: #{v}"
  end
end

fn main(console: Console)
  for input in [" 12 ", "", "  ", "abc", "-4", "0"]
    console.print(describe(input))
  end
end

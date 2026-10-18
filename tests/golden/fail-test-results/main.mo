module Main

enum ParseError
  Empty
  Bad(text: Text)
end

fn parse(text: Text) : Result(Int, ParseError)
  examples
    parse("7") == Ok(7)
    parse("") == Error(Empty)
    parse("x") == Error(Bad(text: "x"))
  end

  return Error(Empty) if text.empty?
  case text.to_int
    Some(n): Ok(n)
    None: Error(Bad(text: text))
  end
end

fn half(n: Int) : Option(Int)
  examples
    half(8) == Some(4)
    half(7) == None
  end

  if n % 2 == 0: Some(n / 2) else: None
end

test "a parsed number is read back"
  assert parse("42") is Ok(n)
  assert n == 42
end

test "a bad number names its text"
  assert parse("4x") == Error(Bad(text: "4x"))
  assert (half(10) or 0) == 5
end

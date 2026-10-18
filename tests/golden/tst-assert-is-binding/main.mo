module Main

enum QuantityError
  Negative(value: Int)
  NotANumber
end

fn parse_quantity(text: Text) : Result(Int, QuantityError)
  case text.trim.to_int
    None: Error(NotANumber)
    Some(n) if n < 0: Error(Negative(value: n))
    Some(n): Ok(n)
  end
end

test "a quantity is read past spaces"
  assert parse_quantity(" 12 ") is Ok(n)
  assert n == 12
  assert parse_quantity("-1") == Error(Negative(value: -1))
  assert parse_quantity("x") == Error(NotANumber)
end

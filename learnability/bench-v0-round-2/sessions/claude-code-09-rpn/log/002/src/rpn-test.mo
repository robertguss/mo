## Tests of the Rpn module.
module RpnTest
use Rpn{RpnError}

test "operators take the two values below them"
  assert Rpn.evaluate("3 4 + 2 *") == Ok(14)
  assert Rpn.evaluate("10 2 8 * + 3 -") == Ok(23)
  assert Rpn.evaluate("2 3 4 * +") == Ok(14)
  assert Rpn.evaluate("42") == Ok(42)
end

test "the left value is the deeper one"
  assert Rpn.evaluate("7 2 -") == Ok(5)
  assert Rpn.evaluate("7 2 /") == Ok(3)
end

test "division rounds toward zero"
  assert Rpn.evaluate("-7 2 /") == Ok(-3)
  assert Rpn.evaluate("7 -2 /") == Ok(-3)
end

test "tokens are separated by any white space"
  assert Rpn.evaluate("  1\t2   + ") == Ok(3)
end

test "an expression with no single value is an error"
  assert Rpn.evaluate("") == Error(Empty)
  assert Rpn.evaluate("   ") == Error(Empty)
  assert Rpn.evaluate("1 2") == Error(LeftOver(count: 2))
  assert Rpn.evaluate("1 2 3 +") == Error(LeftOver(count: 2))
end

test "errors name the token where they happened"
  assert Rpn.evaluate("1 +") == Error(MissingOperand(operator: "+", position: 2))
  assert Rpn.evaluate("*") == Error(MissingOperand(operator: "*", position: 1))
  assert Rpn.evaluate("1 2 ^") == Error(UnknownToken(token: "^", position: 3))
  assert Rpn.evaluate("1 0 /") == Error(DivisionByZero(position: 3))
  assert Rpn.evaluate("1 x +") == Error(UnknownToken(token: "x", position: 2))
end

test "errors are described in words"
  assert Rpn.describe(Empty) == "nothing to evaluate"
  assert Rpn.describe(UnknownToken(token: "^", position: 3)) == "token 3: unknown token ^"
  assert Rpn.describe(MissingOperand(operator: "+", position: 2)) == "token 2: + needs two values"
  assert Rpn.describe(DivisionByZero(position: 3)) == "token 3: division by zero"
  assert Rpn.describe(LeftOver(count: 2)) == "2 values are left over"
end

test "main evaluates each line"
  console = Console.fake_input(["3 4 + 2 *", "1 0 /", "-7 2 /"])
  assert Rpn.main(console) == 1
  assert console.printed == ["14", "error: token 3: division by zero", "-3"]
end

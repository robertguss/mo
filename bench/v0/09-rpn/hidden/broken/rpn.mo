## Evaluates arithmetic written in reverse Polish notation, one expression on each line.
module Rpn
expose RpnError(..), evaluate, describe, main

## Why an expression has no value. Positions count the tokens from 1.
enum RpnError
  Empty
  UnknownToken(token: Text, position: Int)
  MissingOperand(operator: Text, position: Int)
  DivisionByZero(position: Int)
  LeftOver(count: Int)
end

fn apply(operator: Text, a: Int, b: Int, position: Int) : Result(Int, RpnError)
  examples
    apply("-", 7, 2, 1) == Ok(5)
    apply("/", -7, 2, 1) == Ok(-3)
    apply("/", 1, 0, 4) == Error(DivisionByZero(position: 4))
  end

  case operator
    "+": Ok(a + b)
    "-": Ok(a - b)
    "*": Ok(a * b)
    _: if b == 0: Error(DivisionByZero(position: position)) else: Ok(a / b)
  end
end

## The value of `text`, tokens separated by white space.
fn evaluate(text: Text) : Result(Int, RpnError)
  examples
    evaluate("3 4 + 2 *") == Ok(14)
    evaluate("") == Error(Empty)
    evaluate("1 +") == Error(MissingOperand(operator: "+", position: 2))
    evaluate("1 2") == Error(LeftOver(count: 2))
  end

  var stack = []  # the top of the stack first
  for (index, token) in text.words.enumerate
    position = index + 1
    if token == "+" or token == "-" or token == "*" or token == "/"
      case stack
        [a, b, ..rest]:
          value = try apply(token, a, b, position)
          stack = rest.prepend(value)
        _: return Error(MissingOperand(operator: token, position: position))
      end
    else
      case token.to_int
        Some(n): stack = stack.prepend(n)
        None: return Error(UnknownToken(token: token, position: position))
      end
    end
  end
  case stack
    []: Error(Empty)
    [value]: Ok(value)
    _: Error(LeftOver(count: stack.size))
  end
end

fn describe(error: RpnError) : Text
  examples
    describe(LeftOver(count: 2)) == "2 values are left over"
  end

  case error
    Empty: "nothing to evaluate"
    UnknownToken(token: t, position: p): "token #{p}: unknown token #{t}"
    MissingOperand(operator: o, position: p): "token #{p}: #{o} needs two values"
    DivisionByZero(position: p): "token #{p}: division by zero"
    LeftOver(count: n): "#{n} values are left over"
  end
end

fn main(console: Console) : Int
  var status = 0
  while console.read_line is Some(line)
    case evaluate(line)
      Ok(value): console.print("#{value}")
      Error(error):
        console.print("error: #{describe(error)}")
        status = 1
    end
  end
  status
end

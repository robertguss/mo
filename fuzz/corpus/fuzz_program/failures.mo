module Main
enum Inner(T)
Problem(T)
end
enum Outer(U)
Wrapped(Inner(U))
Named(value: Text)
end
fn lift(value: Result(Int, Inner(T))) : Result(Text, Outer(T))
number = try value
Ok(number.to_text)
end
fn first(values: List(Int)) : Option(Int)
Some(try values.first)
end
fn negate(value: Option(T)) : Option(T) where T: Number
Some(- try value)
end
enum TupleError
Mismatch((Option(Int), Option(Bool)))
TupleWrap((Option(Int), Option(Int)))
Scalar(Int)
end
fn inferred() : Result(Int, TupleError)
empty = None
Ok(try Error((empty, empty)))
end
fn scalar() : Result(Int, TupleError)
Ok(try Error(17))
end
fn main() : Int
reader = fn(values) values.get(0) or 17 end
reader([5]) + (first([8]) or stop "empty") + case lift(Error(Problem(true)))
Ok(text): text.byte_count
Error(Wrapped(Problem(_))): 1
Error(Named(value: _)): 2
end
end

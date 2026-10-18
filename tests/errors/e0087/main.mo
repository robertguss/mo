module Main
enum Outer
Named(value: Text)
end
fn helper(value: Result(Int, Text)) : Result(Int, Outer)
Ok(try value)
end
fn main()
end

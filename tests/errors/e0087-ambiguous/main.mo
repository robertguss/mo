module Main
enum Outer
First(Text)
Second(Text)
end
fn helper(value: Result(Int, Text)) : Result(Int, Outer)
Ok(try value)
end
fn main()
end

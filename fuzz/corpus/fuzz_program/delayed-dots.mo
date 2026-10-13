module Main
struct P
to_text: Int
apply: fn(Int) : Int
end
fn main() : Int
field = fn(value) value.to_text end
invoke = fn(value) value.apply(4).abs end
method = fn(value) value.abs end
p = P(to_text: 7, apply: fn(x) x + 1 end)
field(p) + invoke(p) + method(-3)
end

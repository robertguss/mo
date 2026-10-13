module Main
struct Apply(T)
value: T
f: fn(T) : Int
end
fn run() : Int
record = Apply(f: (fn(p) p.0 end), value: (7, "a"))
record.f(record.value)
end

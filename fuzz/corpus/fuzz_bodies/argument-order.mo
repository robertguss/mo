module Main
fn apply(f: fn(T) : Int, p: T) : Int
  f(p)
end
fn run() : Int
  apply(fn(p) p.0 end, (1, "a"))
end

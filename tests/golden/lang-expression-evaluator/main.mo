module Main

enum Expr
  Num(Int)
  Add(left: Expr, right: Expr)
  Mul(left: Expr, right: Expr)
  Neg(Expr)
end

fn eval(e: Expr) : Int
  case e
    Num(n): n
    Add(left: l, right: r): eval(l) + eval(r)
    Mul(left: l, right: r): eval(l) * eval(r)
    Neg(inner): 0 - eval(inner)
  end
end

fn render(e: Expr) : Text
  case e
    Num(n): "#{n}"
    Add(left: l, right: r): "(#{render(l)} + #{render(r)})"
    Mul(left: l, right: r): "#{render(l)} * #{render(r)}"
    Neg(inner): "-#{render(inner)}"
  end
end

fn main(console: Console)
  e = Add(left: Num(2), right: Mul(left: Num(3), right: Neg(Num(4))))
  console.print(render(e))
  console.print("#{eval(e)}")
end

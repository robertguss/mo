import MoCore.Text.Prim

namespace MoCore.Text

def paramsOut (ps : List (String × Ty)) : Sexp :=
  seq (ps.map fun (n, t) => seq [.str n, tyOut t])
def paramsIn := asList (asPair asString tyIn)

partial def exprOut : Expr → Sexp
  | .lit l => tag "lit" [litOut l]
  | .var n => tag "var" [.str n]
  | .global n => tag "global" [.str n]
  | .let_ n e b => tag "let_" [.str n, exprOut e, exprOut b]
  | .lam ps b => tag "lam" [paramsOut ps, exprOut b]
  | .app f xs => tag "app" [exprOut f, seq (xs.map exprOut)]
  | .call n xs => tag "call" [.str n, seq (xs.map exprOut)]
  | .tuple xs => tag "tuple" [seq (xs.map exprOut)]
  | .proj e i => tag "proj" [exprOut e, natural i]
  | .con t c xs => tag "con" [tyOut t, .str c, seq (xs.map exprOut)]
  | .case_ e arms => tag "case_" [exprOut e, seq (arms.map fun (c, ns, b) =>
      seq [.str c, seq (ns.map Sexp.str), exprOut b])]
  | .if_ c t f => tag "if_" [exprOut c, exprOut t, exprOut f]
  | .prim p xs => tag "prim" [primOut p, seq (xs.map exprOut)]
  | .stop s => tag "stop" [.str s]
  | .fail e => tag "fail" [exprOut e]
  | .join n ps b r => tag "join" [.str n, paramsOut ps, exprOut b, exprOut r]
  | .jump n xs => tag "jump" [.str n, seq (xs.map exprOut)]

partial def exprIn : Sexp → Parse Expr
  | .node [.atom "lit", l] => return .lit (← litIn l)
  | .node [.atom "var", n] => return .var (← asString n)
  | .node [.atom "global", n] => return .global (← asString n)
  | .node [.atom "let_", n, e, b] => return .let_ (← asString n) (← exprIn e) (← exprIn b)
  | .node [.atom "lam", ps, b] => return .lam (← paramsIn ps) (← exprIn b)
  | .node [.atom "app", f, xs] => return .app (← exprIn f) (← asList exprIn xs)
  | .node [.atom "call", n, xs] => return .call (← asString n) (← asList exprIn xs)
  | .node [.atom "tuple", xs] => return .tuple (← asList exprIn xs)
  | .node [.atom "proj", e, i] => return .proj (← exprIn e) (← asNat i)
  | .node [.atom "con", t, c, xs] => return .con (← tyIn t) (← asString c) (← asList exprIn xs)
  | .node [.atom "case_", e, arms] => return .case_ (← exprIn e) (← asList armIn arms)
  | .node [.atom "if_", c, t, f] => return .if_ (← exprIn c) (← exprIn t) (← exprIn f)
  | .node [.atom "prim", p, xs] => return .prim (← primIn p) (← asList exprIn xs)
  | .node [.atom "stop", s] => return .stop (← asString s)
  | .node [.atom "fail", e] => return .fail (← exprIn e)
  | .node [.atom "join", n, ps, b, r] => return .join (← asString n) (← paramsIn ps) (← exprIn b) (← exprIn r)
  | .node [.atom "jump", n, xs] => return .jump (← asString n) (← asList exprIn xs)
  | _ => throw "invalid expression constructor or arity"
where
  armIn : Sexp → Parse (String × List String × Expr)
    | .node [c, ns, b] => return (← asString c, ← asList asString ns, ← exprIn b)
    | _ => throw "invalid case arm"

def kinds : List (String × FunKind) :=
  [("function", .function), ("test", .test), ("example", .example), ("promise", .promise), ("anchor", .anchor)]
def ctorOut (c : Ctor) : Sexp := tag "Ctor" [.str c.name, paramsOut c.fields]
def ctorIn : Sexp → Parse Ctor
  | .node [.atom "Ctor", n, fs] => return ⟨← asString n, ← paramsIn fs⟩
  | _ => throw "invalid Ctor"
def dataOut (d : DataDecl) : Sexp := tag "DataDecl" [.str d.name, seq (d.ctors.map ctorOut)]
def dataIn : Sexp → Parse DataDecl
  | .node [.atom "DataDecl", n, cs] => return ⟨← asString n, ← asList ctorIn cs⟩
  | _ => throw "invalid DataDecl"
def constOut (c : ConstDecl) : Sexp := tag "ConstDecl" [.str c.name, tyOut c.ty, exprOut c.body]
def constIn : Sexp → Parse ConstDecl
  | .node [.atom "ConstDecl", n, t, b] => return ⟨← asString n, ← tyIn t, ← exprIn b⟩
  | _ => throw "invalid ConstDecl"
def funOut (f : FunDecl) : Sexp := tag "FunDecl"
  [.str f.name, paramsOut f.params, tyOut f.ret, exprOut f.body,
   .str f.module, enumOut kinds f.kind, natural f.constCount]
def funIn : Sexp → Parse FunDecl
  | .node [.atom "FunDecl", n, ps, t, b, m, k, count] => do
      return ⟨← asString n, ← paramsIn ps, ← tyIn t, ← exprIn b, ← asString m, ← enumIn kinds k, ← asNat count⟩
  | _ => throw "invalid FunDecl"
def programOut (p : Program) : Sexp := tag "Program"
  [seq (p.data.map dataOut), seq (p.funs.map funOut), seq (p.consts.map constOut), .str p.entry]
def programIn : Sexp → Parse Program
  | .node [.atom "Program", ds, fs, cs, entry] => do
      return ⟨← asList dataIn ds, ← asList funIn fs, ← asList constIn cs, ← asString entry⟩
  | _ => throw "invalid Program"

def printProgram (p : Program) : String := document (programOut p)
def parseProgram (s : String) : Parse Program := parseDocument s >>= programIn

end MoCore.Text

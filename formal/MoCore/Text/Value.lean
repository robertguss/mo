import MoCore.Text.Program

namespace MoCore.Text

def rootOut : Option String → Sexp
  | none => tag "none"
  | some s => tag "some" [.str s]
def rootIn : Sexp → Parse (Option String)
  | .node [.atom "none"] => pure none
  | .node [.atom "some", s] => return some (← asString s)
  | _ => throw "expected (none) or (some string)"

partial def valueOut : Value → Sexp
  | .int n => litOut (.int n)
  | .bigInt n => litOut (.bigInt n)
  | .byte n => litOut (.byte n)
  | .decimal n => litOut (.decimal n)
  | .float b => litOut (.float b)
  | .bool b => litOut (.bool b)
  | .char c => litOut (.char c)
  | .text s => litOut (.text s)
  | .bytes bs => litOut (.bytes bs)
  | .unit => tag "unit"
  | .range a b => tag "range" [integer a, integer b]
  | .time n => litOut (.time n)
  | .duration n => litOut (.duration n)
  | .tuple vs => tag "tuple" [seq (vs.map valueOut)]
  | .list vs => tag "list" [seq (vs.map valueOut)]
  | .map es => tag "map" [seq (es.map fun (k, v) => seq [valueOut k, valueOut v])]
  | .set vs => tag "set" [seq (vs.map valueOut)]
  | .con c fs => tag "con" [.str c, seq (fs.map valueOut)]
  | .closure env ps b gs => tag "closure" [envOut env, paramsOut ps, exprOut b, envOut gs]
  | .cap c root => tag "cap" [enumOut capTable c, rootOut root]
where
  envOut (env : Env) : Sexp := seq (env.map fun (n, v) => seq [.str n, valueOut v])

partial def valueIn : Sexp → Parse Value
  | .node [.atom "range", a, b] => return .range (← asInt a) (← asInt b)
  | .node [.atom "tuple", vs] => return .tuple (← asList valueIn vs)
  | .node [.atom "list", vs] => return .list (← asList valueIn vs)
  | .node [.atom "map", es] => return .map (← asList (asPair valueIn valueIn) es)
  | .node [.atom "set", vs] => return .set (← asList valueIn vs)
  | .node [.atom "con", c, fs] => return .con (← asString c) (← asList valueIn fs)
  | .node [.atom "closure", env, ps, b, gs] => do
      return .closure (← asList (asPair asString valueIn) env) (← paramsIn ps) (← exprIn b)
        (← asList (asPair asString valueIn) gs)
  | .node [.atom "cap", c, root] => return .cap (← enumIn capTable c) (← rootIn root)
  | s => return (← litIn s).value

def stringsOut (ss : List String) : Sexp := seq (ss.map Sexp.str)
def pairsOut (ss : List (String × String)) : Sexp := seq (ss.map fun (k, v) => seq [.str k, .str v])

def worldOut (w : World) : Sexp := tag "World"
  [stringsOut w.args, pairsOut w.env, stringsOut w.stdin, stringsOut w.stdout,
   stringsOut w.stderr, pairsOut w.files, integer w.now, natural w.seed]
def worldIn : Sexp → Parse World
  | .node [.atom "World", args, env, stdin, stdout, stderr, files, now, seed] => do
      return ⟨← asList asString args, ← asList (asPair asString asString) env,
        ← asList asString stdin, ← asList asString stdout, ← asList asString stderr,
        ← asList (asPair asString asString) files, ← asInt now, ← asNat seed⟩
  | _ => throw "invalid World"

def bugOut : Bug → Sexp
  | .overflow s => tag "overflow" [.str s]
  | .divByZero => tag "divByZero"
  | .stop s => tag "stop" [.str s]
  | .outside s => tag "outside" [.str s]
  | .repeatedKey => tag "repeatedKey"
def bugIn : Sexp → Parse Bug
  | .node [.atom "overflow", s] => return .overflow (← asString s)
  | .node [.atom "divByZero"] => pure .divByZero
  | .node [.atom "stop", s] => return .stop (← asString s)
  | .node [.atom "outside", s] => return .outside (← asString s)
  | .node [.atom "repeatedKey"] => pure .repeatedKey
  | _ => throw "invalid Bug"
def outcomeOut : Outcome → Sexp
  | .ok v w => tag "ok" [valueOut v, worldOut w]
  | .stopped b w => tag "stopped" [bugOut b, worldOut w]
  | .failed d w => tag "failed" [.str d, worldOut w]
  | .jumped j vs w => tag "jumped" [.str j, seq (vs.map valueOut), worldOut w]
  | .wrong s => tag "wrong" [.str s]
  | .outOfFuel => tag "outOfFuel"
def outcomeIn : Sexp → Parse Outcome
  | .node [.atom "ok", v, w] => return .ok (← valueIn v) (← worldIn w)
  | .node [.atom "stopped", b, w] => return .stopped (← bugIn b) (← worldIn w)
  | .node [.atom "failed", d, w] => return .failed (← asString d) (← worldIn w)
  | .node [.atom "jumped", j, vs, w] => return .jumped (← asString j) (← asList valueIn vs) (← worldIn w)
  | .node [.atom "wrong", s] => return .wrong (← asString s)
  | .node [.atom "outOfFuel"] => pure .outOfFuel
  | _ => throw "invalid Outcome"

end MoCore.Text

import MoCore.Text.Observe
import MoCore.Check

namespace MoCore.Text

inductive Selection where
  | entry (args : List Value)
  | test (name : String)
  deriving Repr, Inhabited
structure Run where
  program : Program
  world : World
  selection : Selection
  fuel : Nat
  deriving Repr, Inhabited

def selectionOut : Selection → Sexp
  | .entry vs => tag "entry" [seq (vs.map valueOut)]
  | .test n => tag "test" [.str n]
def selectionIn : Sexp → Parse Selection
  | .node [.atom "entry", vs] => return .entry (← asList valueIn vs)
  | .node [.atom "test", n] => return .test (← asString n)
  | _ => throw "invalid run selection"
def runOut (r : Run) : Sexp := tag "Run"
  [programOut r.program, worldOut r.world, selectionOut r.selection, natural r.fuel]
def runIn : Sexp → Parse Run
  | .node [.atom "Run", p, w, s, n] => return ⟨← programIn p, ← worldIn w, ← selectionIn s, ← asNat n⟩
  | _ => throw "invalid Run"

/-- Host arguments are first-order. Closures can be transported, but cannot be
injected into a checked run: their type and lexical mode cannot be inferred. -/
partial def valueFits (p : Program) (w : World) (t : Ty) (v : Value) : Bool :=
  match t, v with
  | .int, .int n | .duration, .duration n => decide (InInt64 n)
  | .bigInt, .bigInt _ | .float, .float _ | .bool, .bool _ | .char, .char _
  | .text, .text _ | .unit, .unit => true
  | .byte, .byte n => decide (InByte n)
  | .decimal, .decimal n => decide (InDecimal n)
  | .time, .time n => decide (InTime n)
  | .range, .range a b => decide (InInt64 a ∧ InInt64 b)
  | .bytes, .bytes bs => bs.all fun n => decide (InByte n)
  | .tuple ts, .tuple vs => ts.length == vs.length && (ts.zip vs).all fun (t, v) => valueFits p w t v
  | .list t, .list vs => vs.all (valueFits p w t)
  | .set t, .set vs => vs.all (valueFits p w t) && decide (vs.Pairwise fun a b => Value.equiv a b = false)
  | .map k t, .map es => es.all (fun (a, b) => valueFits p w k a && valueFits p w t b) &&
      decide ((es.map Prod.fst).Pairwise fun a b => Value.equiv a b = false)
  | t, .con c vs => match ctorsOf p t >>= ctorFields c with
    | some fs => fs.length == vs.length && (fs.zip vs).all fun ((_, t), v) => valueFits p w t v
    | none => false
  | .cap c, .cap d _ fake => c == d && (fake.all fun id => id < w.fakes.length)
  | _, _ => false

def execute (r : Run) : Report :=
  match checkProgram r.program with
  | .error s => .illTyped s
  | .ok _ => match r.selection with
    | .entry args => match lookupFun r.program r.program.entry with
      | none => .invalidRun "entry not found"
      | some f =>
        if f.params.length != args.length ||
            !(f.params.zip args).all (fun ((_, t), v) => valueFits r.program r.world t v) then
          .invalidRun "entry arguments do not have the parameter types (closures are not host arguments)"
        else observe r.program f.ret (runProgram r.fuel r.program args r.world)
    | .test n => match lookupFun r.program n with
      | none => .invalidRun ("test not found: " ++ n)
      | some f =>
        if f.kind != .test && f.kind != .example then .invalidRun "selection is not a test or example"
        else observe r.program f.ret (runTest r.fuel r.program n r.world)

def automaticEntry (p : Program) : Parse Selection := do
  let f ← match lookupFun p p.entry with
    | some f => pure f | none => throw "entry not found"
  let args ← f.params.mapM fun (_, t) => match t with
    | .cap c => pure (Value.cap c (some ""))
    | _ => throw "entry has non-capability parameters; use a Run file with explicit arguments"
  pure (.entry args)

def programRoundTrip (p : Program) : Bool :=
  match parseProgram (printProgram p) with
  | .ok q => reprStr p == reprStr q && printProgram q == printProgram p
  | _ => false
def exprRoundTrip (e : Expr) : Bool :=
  match parseDocument (document (exprOut e)) >>= exprIn with
  | .ok f => reprStr e == reprStr f && document (exprOut f) == document (exprOut e)
  | _ => false

end MoCore.Text

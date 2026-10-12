/-
  Mo Core: the evaluator.

  `eval fuel P ρ w e` evaluates expression `e` of program `P` in environment
  `ρ` and world `w`, big-step and strict, left to right (reference 13.1).
  Every recursive call spends one unit of fuel, so `eval` is structurally
  recursive on the fuel and is an ordinary Lean function: deterministic and
  total. Running out of fuel gives `outOfFuel`, never a wrong answer.

  The helpers below take the evaluator for the remaining fuel as an argument
  (`ev`), so that they can be reasoned about once for any `ev`.
-/
import MoCore.Prim

namespace MoCore

/-- Binds parameters to argument values, first parameter first. -/
def bindParams (ps : List (String × Ty)) (vs : List Value) : Env :=
  List.zipWith (fun p v => (p.1, v)) ps vs

/-- Binds a case arm's variables to a constructed value's fields. -/
def bindVars (xs : List String) (vs : List Value) : Env :=
  List.zipWith (fun x v => (x, v)) xs vs

/-- Evaluates arguments left to right, then continues with their values. A
stop, a jump or a wrong outcome in an argument ends the evaluation. -/
def evalArgs (ev : Env → World → Expr → Outcome) (ρ : Env) :
    World → List Expr → (List Value → World → Outcome) → Outcome
  | w, [], k => k [] w
  | w, e :: es, k => (ev ρ w e).bind fun v w' => evalArgs ev ρ w' es fun vs w'' => k (v :: vs) w''

/-- Applies a function value to argument values: the closure's body runs in
its captured environment, with its parameters bound. -/
def applyClosure (ev : Env → World → Expr → Outcome) : Value → List Value → World → Outcome
  | .closure ρc ps body, vs, w =>
    if vs.length = ps.length then ev (bindParams ps vs ++ ρc) w body
    else .wrong "a function value applied to the wrong number of arguments"
  | _, _, _ => .wrong "applying a value that isn't a function"

/-- Calls a top-level function: its body runs with only its parameters bound. -/
def callFun (ev : Env → World → Expr → Outcome) (P : Program) (f : String)
    (vs : List Value) (w : World) : Outcome :=
  match lookupFun P f with
  | some fd =>
    if vs.length = fd.params.length then ev (bindParams fd.params vs) w fd.body
    else .wrong s!"{f} called with the wrong number of arguments"
  | none => .wrong s!"no function {f}"

/-- The `i`-th part of a tuple. -/
def project : Value → Nat → World → Outcome
  | .tuple vs, i, w =>
    match vs[i]? with
    | some v => .ok v w
    | none => .wrong "a tuple has no such part"
  | _, _, _ => .wrong "projecting from a value that isn't a tuple"

/-- The variables and body of the arm for constructor `c`. -/
def findArm (c : String) : List (String × List String × Expr) → Option (List String × Expr)
  | [] => none
  | (c', xs, body) :: arms => if c = c' then some (xs, body) else findArm c arms

/-- Runs the arm of a `case` that matches a constructed value. -/
def selectArm (ev : Env → World → Expr → Outcome) (ρ : Env)
    (arms : List (String × List String × Expr)) : Value → World → Outcome
  | .con c vs, w =>
    match findArm c arms with
    | some (xs, body) =>
      if xs.length = vs.length then ev (bindVars xs vs ++ ρ) w body
      else .wrong "a case arm binds the wrong number of fields"
    | none => .wrong s!"no case arm for {c}"
  | _, _ => .wrong "case on a value that isn't constructed"

/-- Runs a join point's body for a jump to it, and again for each jump to it
from its own body (a loop), at most `fuel` times. The body runs in the
environment of the `join`, with the parameters bound to the jump's values.
A jump to another join point passes on outward. -/
def joinLoop (ev : Env → World → Expr → Outcome) (ρ : Env) (j : String)
    (ps : List (String × Ty)) (body : Expr) : Nat → List Value → World → Outcome
  | 0, _, _ => .outOfFuel
  | fuel + 1, vs, w =>
    if vs.length = ps.length then
      match ev (bindParams ps vs ++ ρ) w body with
      | .jumped j' vs' w' =>
        if j' = j then joinLoop ev ρ j ps body fuel vs' w' else .jumped j' vs' w'
      | o => o
    else .wrong "a jump with the wrong number of arguments"

/-- What a `join` does with the outcome of its `rest`: a jump to it runs its
body; anything else is the outcome of the whole `join`. -/
def catchJump (ev : Env → World → Expr → Outcome) (ρ : Env) (j : String)
    (ps : List (String × Ty)) (body : Expr) (fuel : Nat) : Outcome → Outcome
  | .jumped j' vs w' => if j' = j then joinLoop ev ρ j ps body fuel vs w' else .jumped j' vs w'
  | o => o

/-- The evaluator. -/
def eval : Nat → Program → Env → World → Expr → Outcome
  | 0, _, _, _, _ => .outOfFuel
  | n + 1, P, ρ, w, e =>
    match e with
    | .lit l => .ok l.value w
    | .var x =>
      match lookup x ρ with
      | some v => .ok v w
      | none => .wrong s!"unbound variable {x}"
    | .let_ x e body => (eval n P ρ w e).bind fun v w' => eval n P ((x, v) :: ρ) w' body
    | .lam ps body => .ok (.closure ρ ps body) w
    | .app f args =>
      (eval n P ρ w f).bind fun fv w1 =>
        evalArgs (eval n P) ρ w1 args fun vs w2 => applyClosure (eval n P) fv vs w2
    | .call f args => evalArgs (eval n P) ρ w args fun vs w' => callFun (eval n P) P f vs w'
    | .tuple es => evalArgs (eval n P) ρ w es fun vs w' => .ok (.tuple vs) w'
    | .proj e i => (eval n P ρ w e).bind fun v w' => project v i w'
    | .con _ c args => evalArgs (eval n P) ρ w args fun vs w' => .ok (.con c vs) w'
    | .case_ e arms => (eval n P ρ w e).bind fun v w' => selectArm (eval n P) ρ arms v w'
    | .if_ c t f =>
      (eval n P ρ w c).bind fun v w' =>
        match v with
        | .bool true => eval n P ρ w' t
        | .bool false => eval n P ρ w' f
        | _ => .wrong "if on a value that isn't a Bool"
    | .prim p args => evalArgs (eval n P) ρ w args fun vs w' => Prim.eval p vs w'
    | .stop r => .stopped (.stop r) w
    | .fail detail =>
      (eval n P ρ w detail).bind fun v w' =>
        match v with
        | .text s => .failed s w'
        | _ => .wrong "fail with a detail that isn't Text"
    | .join j ps body rest => catchJump (eval n P) ρ j ps body n (eval n P ρ w rest)
    | .jump j args => evalArgs (eval n P) ρ w args fun vs w' => .jumped j vs w'

/-! ## Running programs -/

/-- Fuel for running examples: enough for every sample. -/
def defaultFuel : Nat := 1000000

/-- Evaluates an expression with no variables bound. -/
def run (P : Program) (e : Expr) (w : World := {}) (ρ : Env := []) : Outcome :=
  eval defaultFuel P ρ w e

/-- Whether an expression evaluates to `true`: how examples and tests are checked. -/
def holds (P : Program) (e : Expr) (w : World := {}) (ρ : Env := []) : Bool :=
  match run P e w ρ with
  | .ok (.bool true) _ => true
  | _ => false

/-- The value and world an expression evaluates to, if it returns. -/
def runOk (P : Program) (e : Expr) (w : World := {}) (ρ : Env := []) : Option (Value × World) :=
  match run P e w ρ with
  | .ok v w' => some (v, w')
  | _ => none

end MoCore

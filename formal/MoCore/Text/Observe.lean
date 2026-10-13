import MoCore.Text.Structural

namespace MoCore.Text

/-- Only observable effects, in model insertion order. -/
structure Effects where
  stdout : List String
  stderr : List String
  files : List (String × String)
  folders : List String
  fakes : List CapState
  deriving Repr, Inhabited
def effects (w : World) : Effects := ⟨w.stdout, w.stderr, w.files, w.folders, w.fakes⟩
def effectsOut (e : Effects) : Sexp := tag "effects"
  [stringsOut e.stdout, stringsOut e.stderr, pairsOut e.files, stringsOut e.folders, seq (e.fakes.map stateOut)]
def effectsIn : Sexp → Parse Effects
  | .node [.atom "effects", out, err, files, folders, fakes] => do
      return ⟨← asList asString out, ← asList asString err, ← asList (asPair asString asString) files,
        ← asList asString folders, ← asList stateIn fakes⟩
  | _ => throw "invalid effects"

/-- The wire report contains structural text as a string, avoiding delimiter ambiguity. -/
inductive Report where
  | returned (text : String) (effects : Effects)
  | stopped (bug : Bug) (effects : Effects)
  | failed (detail : String) (effects : Effects)
  | outOfFuel
  | wrong (detail : String)
  | parseError (detail : String)
  | illTyped (detail : String)
  | invalidRun (detail : String)
  | ioError (detail : String)
  deriving Repr, Inhabited
def reportOut : Report → Sexp
  | .returned s e => tag "returned" [.str s, effectsOut e]
  | .stopped b e => tag "stopped" [bugOut b, effectsOut e]
  | .failed s e => tag "failed" [.str s, effectsOut e]
  | .outOfFuel => tag "outOfFuel"
  | .wrong s => tag "wrong" [.str s]
  | .parseError s => tag "parseError" [.str s]
  | .illTyped s => tag "illTyped" [.str s]
  | .invalidRun s => tag "invalidRun" [.str s]
  | .ioError s => tag "ioError" [.str s]
def reportIn : Sexp → Parse Report
  | .node [.atom "returned", s, e] => return .returned (← asString s) (← effectsIn e)
  | .node [.atom "stopped", b, e] => return .stopped (← bugIn b) (← effectsIn e)
  | .node [.atom "failed", s, e] => return .failed (← asString s) (← effectsIn e)
  | .node [.atom "outOfFuel"] => pure .outOfFuel
  | .node [.atom "wrong", s] => return .wrong (← asString s)
  | .node [.atom "parseError", s] => return .parseError (← asString s)
  | .node [.atom "illTyped", s] => return .illTyped (← asString s)
  | .node [.atom "invalidRun", s] => return .invalidRun (← asString s)
  | .node [.atom "ioError", s] => return .ioError (← asString s)
  | _ => throw "invalid report"

def observe (p : Program) (t : Ty) : Outcome → Report
  | .ok v w => match structural p t v with
    | .ok s => .returned s (effects w)
    | .error s => .wrong s
  | .stopped b w => .stopped b (effects w)
  | .failed d w => .failed d (effects w)
  | .outOfFuel => .outOfFuel
  | .wrong s => .wrong s
  | .jumped .. => .wrong "stray jump"

end MoCore.Text

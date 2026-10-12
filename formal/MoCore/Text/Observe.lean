import MoCore.Text.Value
import MoCore.Support.Num
import MoCore.Support.Time

namespace MoCore.Text

def delimited (a b : String) (ss : List String) : String :=
  a ++ String.intercalate ", " ss ++ b

/-- Structural, not customized `to_text`: the differential oracle must expose fields. -/
partial def structural (p : Program) (t : Ty) (v : Value) (nested : Bool := false) : Parse String := do
  match t, v with
  | .int, .int n | .bigInt, .bigInt n | .byte, .byte n => pure (toString n)
  | .decimal, .decimal n => pure (decToText n)
  | .float, .float b => pure ("0x" ++ hex b.toNat 16)
  | .bool, .bool b => pure (if b then "true" else "false")
  | .text, .text s => pure (if nested then quote s else s)
  | .char, .char c =>
      let s := String.singleton c
      pure (if nested then "'" ++ (if c == '\'' then "\\'" else
        (quote s).drop 1 |>.toString |>.dropEnd 1 |>.toString) ++ "'" else s)
  | .bytes, .bytes bs => pure (delimited "bytes[" "]" (bs.map toString))
  | .unit, .unit => pure "Unit"
  | .range, .range a b => pure (toString a ++ ".." ++ toString b)
  | .time, .time n => pure (timeToText n)
  | .duration, .duration n => pure (durationToText n)
  | .tuple ts, .tuple vs =>
      if ts.length != vs.length then throw "tuple value arity"
      pure (delimited "(" ")" (← (ts.zip vs).mapM fun (t, v) => structural p t v true))
  | .list t, .list vs => pure (delimited "[" "]" (← vs.mapM fun v => structural p t v true))
  | .set t, .set vs => pure (delimited "[" "].to_set" (← vs.mapM fun v => structural p t v true))
  | .map k t, .map es => pure (delimited "{" "}" (← es.mapM fun (a, b) => do
      pure ((← structural p k a true) ++ ": " ++ (← structural p t b true))))
  | t, .con c vs =>
      let cs ← match ctorsOf p t with
        | some cs => pure cs | none => throw "value has no constructors"
      let fs ← match ctorFields c cs with
        | some fs => pure fs | none => throw "unknown value constructor"
      if fs.length != vs.length then throw "constructor value arity"
      let fields ← (fs.zip vs).mapM fun ((n, t), v) => do
        let s ← structural p t v true
        pure (if tNameFields && n != "0" then n ++ ": " ++ s else s)
      pure (if fields.isEmpty then c else delimited (c ++ "(") ")" fields)
  | .fn _ _, .closure .. => pure "<function>"
  | .cap c, .cap d root fake =>
      if c != d then throw "capability type mismatch"
      pure (valueOut (.cap c root fake)).render
  | _, _ => throw "value does not match result type"
where
  tNameFields := match t with | .data _ | .fileError | .entry => true | _ => false

/-- Only observable effects, in model insertion order. -/
structure Effects where
  stdout : List String
  stderr : List String
  files : List (String × String)
  fakes : List CapState
  deriving Repr, Inhabited
def effects (w : World) : Effects := ⟨w.stdout, w.stderr, w.files, w.fakes⟩
def effectsOut (e : Effects) : Sexp := tag "effects"
  [stringsOut e.stdout, stringsOut e.stderr, pairsOut e.files, seq (e.fakes.map stateOut)]
def effectsIn : Sexp → Parse Effects
  | .node [.atom "effects", out, err, files, fakes] => do
      return ⟨← asList asString out, ← asList asString err, ← asList (asPair asString asString) files,
        ← asList stateIn fakes⟩
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

import MoCore.Text.Value
import MoCore.Support.Num
import MoCore.Support.Time

namespace MoCore.Text

def delimited (a b : String) (ss : List String) : String :=
  a ++ String.intercalate ", " ss ++ b

/-- Reference 9.2 escapes C0/C1 controls and interpolation, with the value's delimiter. -/
def referenceQuote (s : String) (delimiter : Char) : String :=
  let contents := String.join (s.toList.map fun c =>
    match c with
    | '\\' => "\\\\" | '\n' => "\\n" | '\r' => "\\r" | '\t' => "\\t"
    | c => if c == delimiter then "\\" ++ String.singleton c
      else if c.toNat < 32 || (127 ≤ c.toNat && c.toNat ≤ 159) then
        "\\u{" ++ hex c.toNat ++ "}"
      else String.singleton c)
  String.singleton delimiter ++ contents.replace "#{" "\\#{" ++ String.singleton delimiter

/-- Structural, not customized `to_text`. Reports retain their bit-level Float
transport; referenceText selects reference 9.2's user-visible form. -/
partial def structural (p : Program) (t : Ty) (v : Value) (nested : Bool := false)
    (referenceText : Bool := false) : Parse String := do
  match t, v with
  | .int, .int n | .bigInt, .bigInt n | .byte, .byte n => pure (toString n)
  | .decimal, .decimal n => pure (decToText n)
  | .float, .float b => pure (if referenceText then floatToText (Float.ofBits b) else "0x" ++ hex b.toNat 16)
  | .bool, .bool b => pure (if b then "true" else "false")
  | .text, .text s => pure (if nested then
      if referenceText then referenceQuote s '"' else quote s
      else s)
  | .char, .char c =>
      let s := String.singleton c
      pure (if nested && referenceText then referenceQuote s '\'' else if nested then
        "'" ++ (if c == '\'' then "\\'" else
        (quote s).drop 1 |>.toString |>.dropEnd 1 |>.toString) ++ "'" else s)
  | .bytes, .bytes bs => pure (delimited "bytes[" "]" (bs.map toString))
  | .unit, .unit => pure "Unit"
  | .range, .range a b => pure (toString a ++ ".." ++ toString b)
  | .time, .time n => pure (timeToText n)
  | .duration, .duration n => pure (durationToText n)
  | .tuple ts, .tuple vs =>
      if ts.length != vs.length then throw "tuple value arity"
      pure (delimited "(" ")" (← (ts.zip vs).mapM fun (t, v) => structural p t v true referenceText))
  | .list t, .list vs => pure (delimited "[" "]" (← vs.mapM fun v => structural p t v true referenceText))
  | .set t, .set vs => pure (delimited "[" "].to_set" (← vs.mapM fun v => structural p t v true referenceText))
  | .map k t, .map es => pure (delimited "{" "}" (← es.mapM fun (a, b) => do
      pure ((← structural p k a true referenceText) ++ ": " ++ (← structural p t b true referenceText))))
  | t, .con c vs =>
      let cs ← match ctorsOf p t with
        | some cs => pure cs | none => throw "value has no constructors"
      let fs ← match ctorFields c cs with
        | some fs => pure fs | none => throw "unknown value constructor"
      if fs.length != vs.length then throw "constructor value arity"
      let fields ← (fs.zip vs).mapM fun ((n, t), v) => do
        let s ← structural p t v true referenceText
        pure (if tNameFields && n != "0" then n ++ ": " ++ s else s)
      pure (if fields.isEmpty then c else delimited (c ++ "(") ")" fields)
  | .fn _ _, .closure .. => pure "<function>"
  | .cap c, .cap d root fake =>
      if c != d then throw "capability type mismatch"
      pure (if referenceText then match c with
        | .args => "<Args>" | .env => "<Env>" | .console => "<Console>"
        | .files => "<Files>" | .readFiles => "<ReadFiles>"
        | .clock => "<Clock>" | .random => "<Random>"
        else (valueOut (.cap c root fake)).render)
  | _, _ => throw "value does not match result type"
where
  tNameFields := match t with | .data _ | .fileError | .entry => true | _ => false

end MoCore.Text

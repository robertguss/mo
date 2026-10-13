import MoCore.Text.Run

open MoCore MoCore.Text

def emit (report : Report) : IO UInt32 := do
  (← IO.getStdout).putStr (document (reportOut report))
  pure (match report with
    | .returned .. => 0 | .stopped .. => 1 | .failed .. => 2 | .outOfFuel => 3
    | .parseError .. => 4 | .illTyped .. => 5 | .invalidRun .. => 6 | .wrong .. => 7 | .ioError .. => 8)

def readCore (path : String) : IO (Parse Sexp) := do
  let bytes ← IO.FS.readBinFile path
  pure <| match String.fromUTF8? bytes with
    | none => .error "input is not valid UTF-8"
    | some s => parseDocument s

def options (args : List String) (fuel : Nat) (test : Option String) : Parse (Nat × Option String) :=
  match args with
  | [] => pure (fuel, test)
  | "--fuel" :: n :: rest => do
      let n ← asNat (.atom n)
      options rest n test
  | "--test" :: n :: rest => options rest fuel (some n)
  | _ => throw "expected --fuel N or --test NAME"

/-- Library sum is a left fold with a checked addition at every step. -/
def primitiveSum (b : Base) (acc : Value) : List Value → Outcome
  | [] => .ok acc {}
  | x :: xs =>
      let step := match b with
        | .int => evalIntOp .add [acc, x] {}
        | .bigInt => evalBigOp .add [acc, x] {}
        | .decimal => evalDecOp .add [acc, x] {}
        | .float => evalFloatOp .add [acc, x] {}
        | _ => .wrong "sum requires Number"
      match step with
      | .ok next _ => primitiveSum b next xs
      | other => other

/-- Lowering's three-way comparison uses the model's base order, not its
boolean comparison instruction. Keep this bridge outside the primitive model. -/
def primitiveEval (p : Sexp) (vs : List Value) : Parse Outcome := do
  match p, vs with
  | .node [.atom "order", b], [x, y] =>
      let b ← enumIn bases b
      let o ← match baseOrder b x y with
        | some o => pure o | none => throw "invalid comparison operands"
      let ctor := match o with | .lt => "Less" | .eq => "Same" | .gt => "More"
      pure (.ok (.con ctor []) {})
  | .node [.atom "sum", b], [.list xs] =>
      let b ← enumIn bases b
      let zero ← match b with
        | .int => pure (.int 0) | .bigInt => pure (.bigInt 0)
        | .decimal => pure (.decimal 0) | .float => pure (.float 0)
        | _ => throw "sum requires Number"
      pure (primitiveSum b zero xs)
  | .node [.atom "literalInsert"], [.map es, k, v] =>
      if (mapGet k es).isSome then pure (.stopped .repeatedKey {})
      else pure (evalMapOp .set [.map es, k, v] {})
  | .node [.atom "quotedText"], [.text s] =>
      pure (.ok (.text ((quote s).replace "#{" "\\#{")) {})
  | .node [.atom "quotedChar"], [.char c] =>
      let body := if c == '\'' then "\\'" else if c == '"' then "\"" else
        (quote (String.singleton c)).drop 1 |>.toString |>.dropEnd 1 |>.toString
      pure (.ok (.text ("'" ++ body ++ "'")) {})
  | .node [.atom "structural", t], [v] =>
      let s ← structural {data := [], funs := []} (← tyIn t) v
      pure (.ok (.text s) {})
  | _, _ => pure ((← primIn p).eval vs {})

/-- A test-only batch protocol: a Value tuple holds the table name, the Core
primitive's text, then its arguments. Evaluate the unchanged primitive model. -/
def primitiveQueries : IO UInt32 := do
  let input ← IO.getStdin
  let output ← IO.getStdout
  repeat
    let line ← input.getLine
    if line.isEmpty then break
    let query : Parse Outcome := do
      let v ← parseDocument line >>= valueIn
      match v with
      | .tuple (.text _ :: .text p :: vs) => primitiveEval (← parseSexp p) vs
      | _ => throw "expected primitive name, Core primitive and arguments"
    match query with
    | .error why => throw (IO.userError why)
    | .ok result =>
      match result with
      | .ok v _ => output.putStr ("ok " ++ document (valueOut v))
      | .stopped b _ => output.putStr ("stopped " ++ document (bugOut b))
      | _ => throw (IO.userError "primitive did not return or stop")
  pure 0

def main (args : List String) : IO UInt32 := do
  try
    if args == ["--primitives"] then return ← primitiveQueries
    let request : Except Report Run ← match args with
      | [path] => do pure (((← readCore path) >>= runIn).mapError Report.parseError)
      | p :: w :: opts => do
          let ps ← readCore p
          let ws ← readCore w
          pure <| do
            let p ← (ps >>= programIn).mapError Report.parseError
            let w ← (ws >>= worldIn).mapError Report.parseError
            let (fuel, test) ← (options opts defaultFuel none).mapError Report.invalidRun
            let _ ← (checkProgram p).mapError Report.illTyped
            let selection ← match test with
              | some n => pure (.test n)
              | none => (automaticEntry p).mapError Report.invalidRun
            pure ⟨p, w, selection, fuel⟩
      | _ => pure (.error (.invalidRun "usage: mocore RUN | mocore PROGRAM WORLD [--fuel N] [--test NAME]"))
    match request with
    | .error report => emit report
    | .ok r => emit (execute r)
  catch e => emit (.ioError (toString e))

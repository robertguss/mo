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

def main (args : List String) : IO UInt32 := do
  try
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

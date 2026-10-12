import MoCore.Prim

/-- Decimal binary64 bits in, the model's public Float.toText spelling out. -/
def main : IO Unit := do
  let input ← IO.getStdin
  let output ← IO.getStdout
  repeat
    let line ← input.getLine
    if line.isEmpty then break
    match line.trimAscii.toString.toNat? with
    | some bits => output.putStrLn (MoCore.floatToText (Float.ofBits (UInt64.ofNat bits)))
    | none => throw (IO.userError "expected decimal binary64 bits")

import MoCore.TextChecks

/-- Export the twelve entry runs and the fake-state spec run in Core 0.2. -/
def main (args : List String) : IO Unit := do
  let dir := args.headD "/tmp/mocore-samples"
  IO.FS.createDirAll dir
  for (name, request) in MoCore.TextChecks.samples do
    IO.FS.writeFile (s!"{dir}/{name}.core") (MoCore.Text.document (MoCore.Text.runOut request))

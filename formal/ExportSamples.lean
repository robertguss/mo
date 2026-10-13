import MoCore.TextChecks

/-- Export the twelve entry runs, Fakes, Library and Metadata in Core 0.8; an optional
second directory receives every canonical fuzz seed from the same codec. -/
def main (args : List String) : IO Unit := do
  let dir := args.headD "/tmp/mocore-samples"
  IO.FS.createDirAll dir
  for (name, request) in MoCore.TextChecks.samples do
    IO.FS.writeFile (s!"{dir}/{name}.core") (MoCore.Text.document (MoCore.Text.runOut request))
  if let some fuzzDir := (args[1]? : Option String) then
    IO.FS.createDirAll fuzzDir
    for (name, payload) in MoCore.TextChecks.fuzzSeeds do
      IO.FS.writeFile (s!"{fuzzDir}/{name}.core") (MoCore.Text.document payload)

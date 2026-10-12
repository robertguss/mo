import MoCore.TextChecks

/-- Export the exact native-oracle timing inputs; this is a fixture writer, not a runner. -/
def main (args : List String) : IO Unit := do
  let dir := args.headD "/tmp/mocore-samples"
  IO.FS.createDirAll dir
  for (name, request) in MoCore.TextChecks.samples do
    IO.FS.writeFile (s!"{dir}/{name}.core") (MoCore.Text.document (MoCore.Text.runOut request))

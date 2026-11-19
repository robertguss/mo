# Trust gates: result files

The files behind `docs/status/trust-gates-run.md`, which holds the predictions, the counts and
the decisions. Every file a run wrote is kept, a weak result included.

- `manifest.json`: SHA-256 of the three task-queue files measured, taken from commit `1389bdfb`
  before the first run. `main.mo` and `job-queue.mo` match `learnability/phase6/strength/manifest.json`.
- `results/dry-run/`: U8. For each program, `<name>.gates.json` is `mo test --json --gates` and
  `<name>.list.json` is `mo test --json --gates --list`; `times.tsv` holds each run's wall time
  and exit status.
- `results/strength/`: U10. The `--strength` records of the frozen and the fitted program
  against the working half and the held-back folder.
- `results/mutants/`: U10. `mo test --json --gates --list` of the frozen and the fitted program.
- `fitted/`: the three files the fitting session handed back.
- `transcript/`: the fitting session's brief, transcript and survivor log, and the recount.

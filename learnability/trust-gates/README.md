# Trust gates: result files

The files behind `docs/status/trust-gates-run.md`, which holds the predictions, the counts and
the decisions. Every file a run wrote is kept, a weak result included.

- `manifest.json`: SHA-256 of the three task-queue files measured, taken from commit `1389bdfb`
  before the first run. `main.mo` and `job-queue.mo` match `learnability/phase6/strength/manifest.json`.
- `count.py`: counts the dry run by program and by row from `results/dry-run/`.
- `results/dry-run/`: U8. For each program, `<name>.gates.json` is `mo test --json --gates` and
  `<name>.list.json` is `mo test --json --gates --list`; `times.tsv` holds each run's wall time
  and exit status. `*.list.crashed.json` are the two listings that crashed before `04a62857`.
- `results/plain/`: plain `mo test --json` of the frozen, current and fitted task queue, with
  `/usr/bin/time -v` for the first two.
- `results/strength/`: U10. The `--strength` records of the frozen and the fitted program
  against the working half and the held-back folder; `runs.tsv` gives each run's exit status
  and seconds.
  `recheck/` holds the same four runs made again after the final review's fix to `--strength`.
- `results/mutants/`: U10. `mo test --json --gates --list` of the frozen and the fitted program,
  two gated runs of the fitted program (`fitted.gates.json` beside the `--strength` runs,
  `fitted.gates-idle.json` alone), and its shorter runs against the frozen copy and against an
  unchanged copy of itself.
- `fitted/`: the three files the fitting session handed back.
- `transcript/`: the fitting session's brief, transcript (`transcript.jsonl`, Claude Code's
  stream-json output), survivor log, run outputs and final report, and `recount.md`, the
  independent recount.

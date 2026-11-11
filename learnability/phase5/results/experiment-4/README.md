# Experiment 4

Declared in [`declaration.json`](declaration.json) and the protocol's experiment 4
section before collection. Result: 4/4 against a 3/4 gate, with at least one pass per
client; the phase status in `summary.json` stays `open` because closure is Robert's
decision.

- `slot-1` to `slot-4`, `summary.json`, `inventory.json`: the published record from
  `evaluation/publish.py`, regraded from each retained bundle, scanned, archived,
  extracted and scanned again. `publication-receipt.json` is its receipt.
- `freeze/`: the frozen manifest and the files it names (spec, controls, verification
  logs, build commands, compiler versions), the independent freeze review, the
  launch approval, and `manifest-superseded.json`, the first manifest, which the
  review rejected because the spec had no freeze key.
- `drill.txt`: the 1,000-kill drill (`MO_KILL_COUNT=1000 MO_KILL_SEED=20261110
  bash ci/check.sh drivers`) on the frozen candidate.
- `pause/`: the frozen pause series (`measure_pause.py --freeze`), with each sample's
  logs and checks; the per-sample data folders and source copies are not kept.

Collection note: the four slots ran in order from one runner command per slot. The
first runner was attached to the operator's tool session, which would have stopped it
after two hours, so during slot 1 its loop was paused (slot 1 ran to completion
untouched) and a detached runner launched slots 2 to 4 with the identical command.
Nothing in the collector, grader or freeze changed between slots.

Recorded review notes that did not block: an exception in the ungraded recording
code would count as an execution failure (none occurred); publication counts the
regrade, and here it equals the live verdict for every slot; only the Codex
entrypoint binary is hashed, not its sibling helpers; the drill exercises a sibling
build of the same inputs as the installed `mo`.

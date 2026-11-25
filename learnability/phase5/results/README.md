# M6 collected evidence

Historical Phase 5 evidence archive (see `docs/status/phase5-run.md` in git history
for the full run record, limits, and release decision boundary).

- `drill.json` and `m6-final-drivers.txt`: declared 1,000-kill result, seed 20261110.
- `results.json` and `pause/`: all five debug and five optimized production samples.
- `stage-controls/` and `m6-frozen-stage-controls.txt`: six separate debugger controls; raw DEVELOPMENT label retained. The ordinary-request stop is in the parser VM.
- `m6-freeze/` and `m6-freeze-2/`: original manifests, binary hashes, compiler commands, repair verification and build-input comparison. Absolute paths identify collection inputs, not a portable reproduction installation.
- `experiment-1/`: one interrupted session and three explicitly unlaunched slots; failed instrument experiment.
- `experiment-2-original/`: all four sessions with unchanged original grades, streams, proxy records, sources and publication inventory. Gate: 0/4, required 3/4 including both clients.
- `qualified-analysis/`: separately frozen post-collection diagnostic correction; original manifests remain alongside corrected analyses. No new sessions or gate changes.
- `verification/`: completed check logs, including failed and corrected runs.
- `review.json`: completed independent instrument-review receipt, including the findings later fixed.

Session artifacts were redacted against both service credentials and provider
secrets. Each experiment was scanned, archived, extracted and scanned again;
publication receipts retain archive hashes and scan counts. The committed text
is the extracted/scanned content; private stopped databases and credential
inventories are not published. Missing artifacts are enumerated in each inventory.
Raw streams preserve actual model behavior, failed assumptions and misses.

The final build-input comparison is byte-for-byte against the collection freeze.
It prepares a release decision; no v0.2 tag exists or is authorized by these results.

# Experiment 3: stopped after slot one

Robert approved the frozen four-slot cohort. Slot 1 (Codex) launched at candidate
4023da940ddbf201465881912841bd25a683375d and finished without timing out. It is
retained as a miss. Slots 2–4 were not launched; no replacement was run. This is
an incomplete cohort, not a completed four-slot score. The original experiment
2 result remains **0/4**.

The collector rejected three resumed `sendfile` calls as unsupported kernel
transfers, although its descriptor tracker recorded both operands as regular
files and no shared-descriptor ambiguity. The `unfinished` condition in
`origin_descriptors.py` rejects these calls. This is a capture coverage defect;
no instrument repair or scoring-rule change was made after launch. Collection
stopped under the predeclared instrument-defect rule.

Independent protected regrading completed in 69.876 seconds and confirmed:

- Passed: task state, activation identity, immutable source tests.
- Missed: command origin, final answer/revision observation, traffic completeness,
  model provenance. These are missing evidence, not proof of incorrect state or
  a substituted model. Capture rejection prevents trusted revision observation.

The complete sealed session, raw trace, credentials and independent regrade stay
in protected machine-local storage. This directory contains the stop report and
freeze metadata only. The four-slot publisher was not run because three slots
are unlaunched; publication integrity and a complete public cohort score are not
claimed. Paths in freeze metadata identify private retained inputs; their contents
are not published here.

The independent freeze review passed before launch and had found and prompted a
fix for concurrent collectors resetting an active session. It did not detect this
resumed-transfer coverage defect. All earlier failed resource measurements remain
recorded. M6 pause and kill-drill inputs differ from this candidate and cannot be
carried forward. Phase 5 and release approval remain open. A future experiment
requires its own declaration; this slot cannot be replaced.

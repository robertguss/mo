# Phase 4 pilot findings

The pilot campaign ended at its 30-session cap. It supports building better diagnostic access and revising the evaluation; it does not measure the effect of Phase 4 instruments, which were not available.

## Evidence and limits

The [ledger](ledger.md) retains all attempts and report revisions. All 29 completed sessions received successful harness grades. Pilot 7 was interrupted by the operator; its records remain private and it is not a difficulty failure. Pilot 3 has a recovered manifest with unknown fields retained as null. Later verdicts are agent recommendations, not individual Robert approvals.

The two question variants met their calibration rule. The four bug variants remained uncalibrated at the cap: none of their completed pilots failed. Hardening changed operator reports only, not source, fixtures or grading criteria. Three final report revisions were not piloted; the fourth was solved in pilot 30. These results do not establish adequate scenario difficulty.

No independent baseline was run. Repeated calibration attempts and report changes cannot be treated as independent measured-baseline observations. The original [protocol](../protocol.md), including its pre-pilot predictions, remains an honest record of the intended experiment; its baseline-before-implementation requirement was superseded on 2026-11-05.

## What the sessions reveal

Agents could often solve the supplied tasks through source inspection, existing service calls, logs, regression tests or simulations. The campaign therefore leaves difficult diagnostic utility unmeasured. It neither establishes that new tools improve solve rates nor establishes that they are unnecessary.

The ledger also records work that supported instruments can make direct: manual saved-state and history decoding in pilots 1, 5 and 12, and repeated author-tool paging in pilot 2. These observations motivate live state views, consistent exports, pure queries and stopped-folder history. They are workflow evidence, not a measured speed or call-count improvement.

Failure replay and original-first revision checks should supply bounded evidence for diagnosis and regression work. Their value must be evaluated on incidents where runtime state, timing, failure context or retention boundaries matter. More difficult scenarios should remain realistic and permit ordinary source access; hiding source or inserting obscurity would build the desired result into the test.

## Consequence for Phase 4

The [canonical phase plan](../../../docs/plans/2026-11-03-1704-feat-phase4-see-inside-plan.md) proceeds from these findings to live inspection and state export, then retained failure diagnostics, then offline query/history/replay. Deterministic fixtures establish correctness and limits; native-client smoke runs establish discovery and usable access. Neither is retrospectively counted as a fresh-agent evaluation.

**2026-11-07:** U3–U5 and U7–U9 have since landed (#294–#297, #301, #302). U6 declared evaluation remains open. The instruments were not available during these pilots.

Before scored evaluation, a separately named diagnostic protocol must declare its questions, expected evidence, grading, allocation and release thresholds. Comparative improvement claims additionally need a prospective independent baseline and appropriate analysis. The old predictions and every unsuccessful or incomplete observation stay recorded. Release still depends on the new declared gate and Robert’s decision.

Public findings contain no private defect descriptions, fixture contents, answer keys or reports. Public grade/manifest records and the ledger retain the available provenance; full transcripts and variant details stay in their private locations.

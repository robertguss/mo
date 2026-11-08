# Independent instrument review

Before collection, ce-code-review examined the staged M6 instruments against
814ddd0. Seven local lenses and an independent Claude peer completed; the peer
requested and reported claude-opus-5-5, with requested high effort (actual effort
unverified). This was code review, not an evaluation session. The original
receipt is retained at `/tmp/compound-engineering-1000/ce-code-review/m6-independent/review.json`.

Seven findings were confirmed and corrected:

| Finding | Correction and check |
| --- | --- |
| Quoted Codex shell push missed | Shared command recognizer; bare, quoted, wrapped, argv-list and non-push controls; real proxy attestation. |
| Claude string-valued message crashes grading | Phase 4 content-block reader; refusal followed by successful push control. |
| Cleanup error could retain success | Independent cleanup attempts and final verdict after reset; real injected-reset-failure control. |
| Compiler failures lose evidence | Persist timeout streams and malformed-output attempt ledger; two failure-retention tests. |
| Declared service log inaccessible | Operator-owned read-only session log, prompt path, OS readability/non-writability checks. |
| Measurement timeouts lose partial output | Shared capture helper with timeout output and elapsed-command receipt. |
| Provider secrets optional in publisher | Separate required nonempty provider inventory; missing-inventory refusal and encoded-secret scans. |

Independent targeted validation found one remaining startup edge: eager lookup
of an unset service process during cleanup after Popen failure. Guarded callbacks
correct it. The validator reproduced the original failure and then verified
status 1, retained failure manifest, working-source capture and both reset calls.
The durable regression is in `evaluation/test_harness.py`; all eight runner tests
pass. The original seven findings and the follow-up remain in the review record;
this is not described as a no-findings review.

The full OS collector control passed success, timeout and injected-reset-failure
cases in 272.800 seconds (`build/m6-collector-cleanup-control.txt`), including
source tests, live/restart grades, credential routing, isolation, log access and
cleanup. Four proxy tests and five publication tests passed. The independent
validator also ran lightweight proxy, publisher, runner and failure-retention
checks and found no further consequential defect in that bounded scope.

Final drill, timing collection and agent outcomes were outside this review's
scope. No release decision was made.

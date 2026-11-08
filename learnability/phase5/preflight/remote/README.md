# Remote upgrade tools

The catalog describes the three upgrade operations, both credentials, the slot
lifetime and consecutive bounded-page protocol. Ordinary views stay read-only;
upgrade open/page/commit are the documented exception. Active-source export is
explicitly deferred in `docs/design/upgrades.md`: a source bundle alone omits
historical MOCC context required to rehearse some active programs.

## Predictions and focused evidence

The catalog check was strengthened before editing its descriptions. It failed
on `mo-upgrade-page`, whose provisional description did not name
`Mo-Upgrade-Authorization`; after the edit `ci/check_operator.py` passed against
GCC debug. Other tests were developed alongside the changes, not presented as
red-before-implementation evidence.

`ci/check_upgrade_remote.py` uses a fresh instance for each direct, HTTP recording
proxy, TCP forwarder and rate-limited proxy case. It compares all stdout/stderr
bytes for an actual activation and a missing-migration refusal. Wrong service
credentials get exactly the author-tool HTTP refusal; missing/wrong upgrade
credentials get E1414. A following valid open succeeds, proving the refused calls
did not reserve the slot. The proxy returns one synthetic no-effect rate refusal, shaped exactly like the
previously observed real server reply, without forwarding that page. It requires
the retried request bytes to match, then forwards the page and permits commit.
The server's real rate accounting remains covered by `test_protocol_server`'s `rate` test. The resulting reports match the direct case.
Recordings contain only tool names, HTTP status, a rate-refusal boolean and
whether the synthetic response was forwarded.

The embedded runtime test appends `mo-future-view` after the three upgrade names
and verifies it is not classified as an upgrade. It also pins all three upgrade
classifications and distinguishes no-effect rate refusals from applied/unknown
ones. Native routing invokes that name-based classifier instead of an index range.
A second regression in `test_protocol_server` appends a synthetic ordinary view
through compile-time source substitution and sends a real MCP request through
the native server, requiring the view response and unchanged service state. A separately linked
negative-control binary restored index-range routing; it failed exactly the
three view-success/no-credential-refusal assertions (exit 1). The production
source and object were untouched.
The views fuzzer now exercises upgrade validation/extraction from arbitrary JSON
and the push response reader from raw HTTP, with accepted upload argument seeds
and a rate-refusal seed.

Frozen client names, observed calls and approval behavior are in
[CLIENTS.md](CLIENTS.md), including the Claude model-fallback limitation. These
are catalog/routing probes, not the agent evaluation.

## Failed assumptions retained

- The first full sanitizer/drivers gate failed the rate-refusal assertion for
  all executables under concurrent load. The initial focused GCC run had observed
  a real refusal, but 50 sequential calls do not guarantee exhaustion: a slow
  build can process fewer than 20 calls per second and continuously refill its
  bucket. The corrected driver injects one explicit no-effect proxy reply and
  checks the identical retry rather than weakening the assertion. The failed
  gate log is `build/check-m5-sanitize-drivers-run1.txt`; it is not a pass.

- A TCP forwarder on a different local port initially returned HTTP 421. Unlike
  the recording proxy, a tunnel preserves Host. The existing authority check
  requires the listener port or an explicit `--allow-host` authority. The runbook
  now uses the same port on the operator and host; the local forwarder driver
  explicitly allows its different local authority. No loopback or authority rule
  was relaxed.
- The first forwarder implementation did not preserve half-close. Its corrected
  relay propagates EOF in one direction while continuing to read the other.
- A lower-test run overlapped formatting of the embedded Mo sources and failed
  the disk/embedded-source equality checks. This was test/build ordering, not a
  successful verification; subsequent checks use stable source files.

## Limits and unchanged residuals

The forwarding test is a local TCP equivalent, not a real remote SSH deployment.
SSH authentication/encryption and a second machine are not claimed. Sustained
rate starvation still exhausts the command deadline and can leave a slot until
its 60-second idle expiry; only explicit no-effect rate replies are retried.
Lost replies are not automatically retransmitted. The duplication of decimal/
hex readers and push-reader plumbing remains; this milestone does not change
those parsing contracts. Live pause measurement on 5,000 jobs, the drill,
agent evaluation, release and E1428's deterministic test remain outside scope.
The live, M2 and rehearsal evidence and all their failed assumptions remain intact.

## Verification

Focused observations, including review corrections:

- GCC debug build and `ci/check_operator.py`: passed.
- `ci/check_upgrade_remote.py`: passed direct/proxy/forward/throttled comparisons
  after the deterministic retry correction in GCC and ASan/UBSan; each ran
  concurrently with `ci/check_operator.py`, which also passed.
- Views fuzz smoke: sanitizer-enabled, seed 5011108, 10-second time budget;
  completed 3,332 executions in 11 seconds. A final rebuilt run with seed
  5011109 completed 2,119 executions in 11 seconds after the review changes.
  These are finite exploration, not a proof.
- `test_lower` (including appended-view routing and rate-reader controls): passed.
- Native `test_protocol_server`: GCC and ASan/UBSan passed after the review correction; the
  index-range routing mutant failed three assertions as expected.
- Initial `bash ci/check.sh gcc`: `check.sh finished: pass`.
- Final `MO_ALL_DRIVERS=1 bash ci/check.sh gcc sanitize drivers static`:
  `check.sh finished: pass`, with no skips. This reran GCC and sanitizer units,
  drivers against GCC debug, Clang debug and Clang ASan/UBSan, and drivers/static
  after the guide edit. All three remote-driver checks passed (12, 13 and 20
  seconds respectively). The local log is `build/check-m5-final-run2.txt`.
- `make deps tidy format-check`: exit 0; clang-format 14.0.6.
- Build logs are local and untracked; the commands and bounded observations here
  are the durable record. No nightly, remote SSH deployment, drill or evaluation
  success is claimed.

## Independent review

The completed ce-code-review run `20261108-194258-a9e6779e` covered correctness,
security, testing, project standards, reliability, API contracts, maintainability
and agent use. Its confirmed finding was that the remote driver bypassed the
check runner's failure aggregation. It now uses `later`, and the failed first
full gate correctly reported all three remote-driver failures. The review also
prompted the native dispatch regression and explicit Bearer header syntax.
No cross-provider adversarial pass was run; no additional concrete material
failure scenario was identified beyond the specialist checks.

The latest nightly inspected during this work failed on the earlier M3 commit
`2e0e1a8a1b527fed048e98f5d0e2535d4ee9ea53` in Phase 4 replay/grader checks
([run](https://github.com/robertguss/mo/actions/runs/42489339400)). This work does
not claim to resolve that nightly or run the out-of-scope agent evaluation.

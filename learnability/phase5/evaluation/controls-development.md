# Development controls

These are preparation checks, never session evidence. No agent was launched.
The reference predicts exact preservation of every job wire object, including
payloads, attempts and lease deadlines, and revision 1 before and after a
folder-only restart. Every negative predicts rejection.

Build: `make -j8 CC=gcc BUILD=build/eval-controls` (completed successfully).
Test: `python3 -m unittest discover -s learnability/phase5/evaluation -p test_grade.py -v`.

The first run failed all starts because the data directory did not exist; the
launcher now creates it. The next run accepted the reference and rejected
unchanged, dropped job, altered payload, an extra completed job (the leased job
changed to Done), wrong revision, and source-only/no push. Initializer substitution
failed compilation due to an unused migration parameter; it was corrected to
`_old` before rerunning. Local logs: `/tmp/mo-eval-controls.log` and
`/tmp/mo-eval-controls-final.log`.

Runner API: `fixture.build` makes a protected populated stopped folder and old
source; `precheck(client, expected)` checks its jobs; `live_grade(client)` captures
live observations; after stopping the service, `stopped_grade(...)` restarts from
only the folder, checks jobs/revision and structured activation history. Supply
operator-owned base/candidate MOSB digests from `mo check --upgrade-from --json`
in expected, and the extracted final answer. `source_precheck` copies source and
restores queue-tests before running tests. `attestation` consumes private stream
and proxy files; development pushes are separately labeled and accepted only
with explicit development=True. Neither credential is a literal fixture value.

Limitations: the tests perform operator development pushes directly, not agent
pushes through the recording proxy. Stream/proxy attestation and source_precheck
still need runner integration tests. The fixture retains old source and the data
folder's recorded bundles; it does not export a separate MOSB/MOCC artifact.
This initial service has no historical migration context. Expected files must
remain outside the session account, as required by the runner's isolation contract.
The duplicate-completion control checks an additional job becoming Done, not an
external side effect. Final collection and freeze remain the parent's gates.

Final rerun completed: one unittest with eight real-service subcases, 42.850s,
OK. Reference accepted; all seven negatives rejected after live and restart
observations. All service processes were stopped in finally blocks; no tests or
builds remain running. Source precheck and stream attestation were not included
in these eight subcases. No final evidence was collected.

Parent integration: `python3 -m unittest discover -s
learnability/phase5/evaluation -p test_proxy.py -v` passed three tests in 3.683s.
The added test performs a real production `mo push` through the recording proxy,
then pairs its recorded requests/replies with an explicitly synthetic DEVELOPMENT
Codex command event. Accepted attestation identifies the candidate digest. Missing
push command, failed command, and a deliberately modified refused commit report
are rejected. These are instrument controls, not agent sessions. Existing both-token
forwarding/redaction and command-origin tests passed in the same run.

## Stable lease fixture and trusted saved-state checks

The original fixture leased at wall-clock time, with a 30-minute deadline. That
was incompatible with a 60-minute session and with freezing four populated
folders in advance: natural expiry could masquerade as a migration defect.
No scored session was run under that assumption.

Preparation now starts a temporary tool-only source variant. Its fixture_lease
handler invokes the unchanged JobQueue.lease with 2099-01-01T00:00:00Z and the
ordinary 30-minute lease duration. After normal submits, acknowledgement and a
failed attempt, the outstanding lease is therefore due at
2099-01-01T00:30:00Z. A no-migration tool-only push then restores the exact
ordinary main.mo and job-queue.mo before freezing. The seeding tool is absent
from the active source. Existing handlers, lease duration and immutable source
tests are unchanged. Preparation writes its check, push and history reports.
It adds one recorded activation, not an evaluation success. expected.json freezes
that exact preparation history prefix; grading requires it unchanged plus one
session activation. No UpgradeOld dependency or historical migration was added:
original source alone successfully checks and pushes the reference migration.
No additional MOCC artifact is required for this fixture's migration.

A deterministic clock control exports the real prepared state, then runs the
ordinary JobQueue.expire over it offline at the end of 2098 and at the exact
2099 deadline. The former must leave every job unchanged; the latter must change
the outstanding lease. It also asserts both restored source files are byte-identical
to the ordinary queue. Initial control attempts incorrectly used query --data
without a failure id, then C-style &&; both were corrected to a state export and
Mo's and operator. Raw results remain in build/eval-aging-development.txt and
build/eval-aging-query-development.txt. The corrected standalone clock control
passed in 5.946 seconds. The first full aging suite then passed two tests,
including all eight real-service migration cases.

Grading now also reads the trusted mo-state operator view, requiring complete
state both live and after folder-only restart. It compares the actual persisted
revision field, next_id and normalized saved job records, not merely author-tool
answers. Expected next_id is independently fixed at 5 after four submissions.
New controls provide a constant revision tool without any stored revision field,
and a migration that increments next_id incorrectly. A dropped-job observation
with a forged correct author-list answer is also rejected by the saved-state check.
These instrument controls remain DEVELOPMENT evidence, never scored sessions.

Final focused command:
`MO_EVAL_BINARY=/tmp/mo-eval-build-aanyuygb/build/gcc/debug/mo python3 -m unittest discover -s learnability/phase5/evaluation -p test_grade.py -v`
passed two tests in 99.219 seconds, including the clock control and ten real-service
cases. Reference passed; unchanged, initializer substitution, dropped job, altered
payload, duplicate completion, wrong revision, source-only, constant-tool/no-field
and wrong-next_id all failed their declared hidden checks. The constant tool was
rejected specifically for missing stored_revision; the wrong-next_id candidate
was rejected specifically for next_id. Live and restarted operator state were
checked. The forged author-list and omitted preparation-prefix checks also passed.
Complete output: build/eval-aging-stored-state-development.txt. No service or test
processes remain running. Collector-wide proof must be rerun against these new
fixture and observation fields before final collection; the prior collector proof
remains evidence for its earlier fixture only.

## Independent review corrections before collection

Review found that shell-quoted Codex commands were missed by push attribution
and a Claude string-valued message could raise before grading a later push.
Both paths now use shared command recognition and the Phase 4 content-block
reader. Four proxy tests, including a real push with both stream shapes, passed
in 3.626 seconds (`build/m6-proxy-review-fixes-2.txt`).
Publication now requires separate service-token and provider-secret inventories;
a missing provider inventory fails closed. Five publication tests passed.
No model rule, pass gate or scored session changed.

# Task queue drill development

Declared before execution: baseline 2 kills, seed 8101; extended development
15 kills, seed 8102, GCC debug. Focused corrections repeat seed 8102 with
15 kills, retaining failures here. These runs are excluded from final evidence.
The final 1,000-kill collection remains blocked pending merged, verified nightly
repair; the recorded main baseline is d31c9301.

Evidence strategy: characterize the existing reply model with the baseline,
then exercise every new candidate/refusal in a fixed prelude before randomized
kills. Random delays identify requests in flight, not internal migration stages.
Production sizing at 5,000 jobs and 500 leases is not established by this run.

The GCC debug build passed. Baseline seed 8101 requested two kills but observed
three (one push kill plus the unconditional call kill); it passed with 17 calls,
one push, zero activations, and no hot journal. The loop now checks its kill
budget before the call kill.

Development failures retained, all seed 8102 and requested count 15:

- Run 1 stopped before serving: inserting `use` before `expose` violated syntax.
- Run 2 stopped before serving: old and new nominal `Job` types differ. The
  migration now reconstructs each job and state, preserving its lease deadline.
- Run 3 stopped on the oversized control: diagnostic stdout has multiple JSON
  records, rather than a single upgrade report.
- Run 4 refused the oversized fixture with E1005 (one overlong line), not E1417.
  Bounded comment lines now exceed the bundle bound without hitting that limit.
  Retained folder: `/tmp/mo-kill-loop-2r3dnnvj`.
- Run 5 passed: 15 kills, one hot journal, 14 readable histories and one recovery
  requirement, 285 calls, 276 answered, three no-effect refusals, nine unknown
  outcomes, eight pushes, three activations, one requested/observed push kill.
  E1426, E1423, E1417 and E1416 were each observed once. Elapsed 41 seconds.

The fixed prelude preserves complete job records, including exact lease times,
across field migration and checks a folder-only restart. Random traffic retains
all previous reply-effect modeling, burst and history checks. The migration
rebuilds nominal jobs explicitly; the added revision starts at one. The trap
candidate divides by zero; oversized source exceeds the 1 MiB bundle limit.
One caller sends sequential queue operations concurrently with the push client.
The random model excludes lease expiration (folders live at most 20 minutes;
leases last 30 minutes), retains at most roughly 500 jobs per folder, and uses
short payloads. It does not measure the original 5,000-job, 2,048-byte,
500-lease sizing declaration or prove power-loss durability.

Kill classes are requested call-in-flight, requested push-in-flight, and a fixed
post-migration restart. Actual push kills and hot SQLite journals are counted
separately. No instrument in this driver observes compilation, migration,
validation or activation transaction entry; those internal stage counts remain
unobserved. Random sleep delays must not be reported as stage coverage. Final
collection and internal-stage coverage remain open for integration; no final
collection or nightly success is claimed.

The fixture/duplicate-detection rerun passed: seed 8102, 15 kills, one hot
journal, 266 calls, 255 answered, two refused, 11 unknown, eight pushes,
three activations and all four deliberate refusal classes. It took 53 seconds.
The subsequent ordinal check closes and rereads a recovered store when the
pre-recovery history could not expose its records, then starts folder-only again.

Ordinal-check rerun passed: seed 8102, 15 kills, zero hot journals, 255 calls,
248 answered, two refused, seven unknown, ten pushes and three activations.
Three push kills were requested and two occurred; twelve call kills and one
post-migration kill occurred. The four refusal classes all occurred (access twice).
Elapsed 46 seconds. This particular run did not exercise recovery-required.

The final fixed prelude additionally requests a kill while the field-migration
push is in flight, reconciles the old or migrated outcome, and retries if the
old program remains. A completed push before the deadline is reported as no
push kill. It still does not identify an internal migration-stage kill.

Final source run passed: seed 8102, requested/observed 15 kills, zero hot
journals, 15 readable histories, 295 calls, 286 answered, three refused and
nine unknown outcomes. Eight pushes produced three activations; all four
refusal codes appeared (E1426 twice). Two push kills were requested but both
pushes completed before their kill deadline, so observed push kills were zero;
fourteen call kills and one post-migration kill occurred. Elapsed 42 seconds.
This is explicitly not evidence of a kill inside migration or its retry path.
`python3 -m py_compile ci/check_kill_loop.py` and `git diff --check` passed.
The existing drivers invocation already runs this extended script; no invocation
change was needed. Authoritative integration tests belong to the parent.

## Explicit debugger controls (development only)

Declared before execution: **3 kills, seed 20261110**. Invocation:
`python3 ci/check_kill_stages.py build/gcc/debug/mo`.
The ordinary driver invocation is unchanged. Missing gdb, attach permission, debug
symbols or an unhit breakpoint fails this additional control; nothing is skipped.

At repaired main `814ddd0f` plus uncommitted M6, all three controls passed.
Output: `build/kill-stages-development.txt`; retained scripts, breakpoint backtraces
and folders: `/tmp/mo-kill-stages-jpvq9ncg`. Output records binary and source SHA256
and requested lines; backtraces record actual symbols and executable lines.
GDB resolves the postcommit macro anchor at line 535 to executable line 546.
These debugger pauses are not ordinary timing or performance measurements.

The controls reuse `Run.judge`, `Run.restart`, history, variants and push retry.
Each begins with an accepted submit and lease, checks complete job records and
lease deadlines after restart, and checks exactly one durable activation after
retry. Migration stops in `migration_state` after old-state decode, immediately
before `mo_call`; this observes the migration stage, not an instruction partway
through the migration VM body. It recovers the old program with zero activation
records and successfully retries. Activation entry has the same outcome.
Postcommit/pre-reply recovers the new program; repeated push succeeds without a
second activation record.

Coverage remains bounded: activation entry is not inside the SQLite transaction;
upload-in-progress, compiled-staged-before-commit, and an observed ordinary-call
execution point are not supplied by this helper. The existing
`check_upgrade_live.staged_kill` uses a two-second wait after raw upload, so it
must not be relabeled proof of completed compilation. The focused
`test_upgrade_store` fault control is run separately, with output retained in
`build/kill-stage-store-control.txt`; it is store-fault evidence, not a process
kill. Thus the full requested six-class observed-stage control is incomplete.
The final declaration remains **1000 kills, seed 20261110**, pending parent freeze;
no final drill or phase closure is claimed.

Parent integration extended the explicit control to six DEVELOPMENT kills, seed
20261110, declared before execution. `python3 ci/check_kill_stages.py
build/gcc/debug/mo` passed all six; output is
`build/kill-stages-six-development.txt`, artifacts `/tmp/mo-kill-stages-wg4yhfm6`.
Upload handling, confirmed staged compilation (step over compile_end and assert
staged), ordinary VM execution, migration after the first VM instruction, an
activation transaction after BEGIN IMMEDIATE, and postcommit/pre-reply were
observed. Migration arms a VM instruction breakpoint only after reaching its
mo_call site, then ignores the first instruction hit. Backtraces preserve actual
stops. Restart/retry retained accepted jobs and lease deadlines and exactly one
activation. The earlier three-point limitations remain historical; this run
supplies the missing observed classes. Debugger controls are separate from the
randomized final drill and performance measurements; no final collection claimed.

Correction to the preceding six-stage claim: inspecting the migration backtrace
found a stale GCC binary with source-line drift; its stop was timer evaluation
(argument_count zero), not migration. Thus that run does not prove the labeled
internal stages. It remains retained. A new control asserts the stopped VM's
mo_call has one argument, and the rerun uses the staged readable binary after
byte-comparing its upgrade/protocol sources with this checkout. Six DEVELOPMENT
kills with seed 20261110 are declared again, not final evidence.

The source-matched rerun passed all six controls with binary SHA256
517da0610ccc341a69dedd202cd8f5439cab695827d8ad60cf2312f8e61703b9.
`build/kill-stages-matched-development.txt` retains the results and artifact root.
Migration now verifies the one-argument mo_call frame after skipping the first
VM instruction; transaction-interior and compiled-staging checks also passed.
The stale-binary run above is not used for those claims. clang-format 14's dry
check passed the probe header and both instrumented C files.

Independent final evidence inspection narrowed the `ordinary_call` debugger
claim: its stop is in the ordinary request parser's PF_SCAN VM invocation, not
the author tool body. The frozen control record is retained unchanged; the run
report states this exact boundary. Randomized ordinary-call kills and the five
other debugger stages remain separate observations.

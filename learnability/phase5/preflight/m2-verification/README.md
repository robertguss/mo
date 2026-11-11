# M2 verification

This milestone covers typed upgrade checking, migration validation and checked
restart. The approved declarations-context clarification is recorded in
[the restart probe](../restart-context/README.md). Streaming notifications and
live swapping remain later milestones.

## Acceptance evidence

- `ci/check_upgrades.py`: typed compatibility and signature refusals; actual
  migrated value (17, distinct from the candidate initializer 999); traps and
  restart-budget exhaustion leave the database byte-identical; a migration
  above 1,048,576 work units but within 16,777,216 succeeds; folder-only restart;
  original generated declarations survive three subsequent tool-only upgrades
  and eviction of the original source; generic and recursive state types;
  context tampering; source identity; history; real v0.1.0 replay and adoption.
- `ci/check_upgrade_fallback.py`: cached declarations and saved-schema synthesis
  after a simulated future compiler rejects an old body; a nonempty tail that
  cannot be replayed is refused without changing the database.
- `ci/check_upgrade_adoption.py`: same-source replay after a modeled historical
  image identity, first-record checksum mismatch naming its record and handler,
  byte-identical refusal, and explicit legacy adoption with zero verified records.
  The identity model tests the rebuild branch, not a second compiler's semantics.
  The separate committed v0.1.0 fixture supplies real old-binary replay evidence.
- `tests/test_service_store.c`: first snapshot insertion succeeds, program-row
  insertion receives injected `SQLITE_FULL`, reopen sees neither half, and retry
  commits both. The existing offline driver catches unwanted log compaction on
  an unchanged-program restart.
- Saved-state, bundle, compilation-context, compiler-child and store tests cover
  framing limits, truncation, explicit nominal overrides, shared native/VM work
  budgets, output preservation, child cancellation/reaping, and transaction
  admission. Diagnostic inventory distinguishes message tests from unreachable
  future live-upgrade codes.

## Fuzzing and bounded proofs

All listed fuzz runs used the sanitizer-enabled targets and ended successfully.
Long front-end runs used `-max_total_time=300`; each completed after 301 seconds.
Their compiler front-end source was unchanged by the later context/report fixes.

| Target | Seed | Executions | Largest generated input allowed by that run |
| --- | ---: | ---: | ---: |
| `fuzz_check` | 3090678051 | 166,807 | 4,096 bytes |
| `fuzz_lower` | 4279140096 | 8,825 | 4,096 bytes |
| `fuzz_codegen` | 1166858704 | 92,451 | 48,397 bytes |
| `fuzz_program` | 2347083855 | 7,461 | 4,096 bytes |

Final boundary smoke runs: bundle 661,186 executions in 11 seconds; saved-state
migration input/output 100,066 in 11 seconds; schema synthesis 177,459 in 11 seconds.
After the shared-reader refactor, compilation context and metadata completed
699,521 executions in 11 seconds. These are finite fuzz observations, not proofs
of all accepted input sizes. Concrete boundary tests cover the declared 1 MiB
bundle/context limit.

Bundle CBMC commands passed both proof and reach checks:

```sh
MO_CBMC_INPUT=18 bash cbmc/prove.sh bundle_path
MO_CBMC_INPUT=18 bash cbmc/prove.sh --reach bundle_path
bash cbmc/prove.sh mo_bundle_read
bash cbmc/prove.sh --reach mo_bundle_read
```

The framing bound is 32 input bytes. String comparisons and UTF-8 validity are
modeled predicates with readable-span assertions; framing models the separately
checked path reader. These establish bounded memory access and refusal output
preservation, not lexical policy for all input lengths. `cbmc/README.md` records
the assumptions. Earlier concrete-comparison attempts timed out at 180 and 300
seconds; neither timeout counted as a proof.

## Failures kept and corrected

The initial integration run passed GCC, Clang and sanitizer units but failed
existing CLI and offline drivers. Source-byte changes now require explicit
upgrade even when the image key is unchanged, so those expectations changed to
E1430. History gained an exact program-identity line. Other failures were real
regressions: the compiler-child path lost ordinary startup diagnostics, the first
service preflight inspected the wrong syntax-tree level, and unchanged-program
restart unnecessarily replaced the snapshot and trimmed the log. Those were
fixed while retaining the existing error and history assertions.

The initial source-context assumption also failed: a retained migration body
cannot be recompiled from only its current state schema after original source
retention expires. The original prediction, failed assumption, reproducer and
approved bounded-context resolution remain in the restart probe above.

## Independent review and corrections

The completed Compound Engineering review (`20261108-050855-cf22caa1`)
used ten native review lenses and an adversarial Claude-provider review. The
requested peer was Claude Opus 5.5 at high effort; the serving model was not
independently attested. All three actionable findings were corrected:

- A build change with the same image key now records an adoption. The regression
  test captures a real failure under the new build identity and verifies that its
  program, source bundle, context and snapshot remain pinned beyond normal
  retention. The historical build digest is modeled; no second compiler's
  semantics are claimed by this test.
- The JSON contract now distinguishes conditional comparison fields from
  always-present work metrics; the driver asserts both report shapes.
- `mo check NEW --upgrade-from DIR` reads the active bundle and validated context
  from a stopped folder. `ci/check_upgrade_context.py` proves that a second
  migration can be checked after three tool-only activations evict the original
  source, then restarts with value 117. Successful checks and refusals preserve
  the database bytes and file list; a live owner and corrupt context are refused.

The last integration attempt also caught noncanonical indentation in the new
recursive/generic fixture: its source excerpt differed after formatting. The
fixture was formatted; the historical v0.1.0 source remains unchanged.

## Verification limits

The Lean model covers finite saved schemas, explicit nominal mapping and the
accepted/refused migration boundary. Recursive source types, runtime resource
limits and SQLite transactions remain executable-test obligations.

The full activation statement-fault matrix belongs to U4. First-start
transaction rollback and legacy nonempty-tail replay are tested here. The review
also identified a missing automated case for changed-source migration after a
nonempty log tail. The added driver changes state from 7 to 8, then requires the
migration to produce 18 (rather than snapshot-derived 17 or initializer 999),
checks the upgrade head is 1 and the consumed tail is empty, then restarts again.
A failed report write after a committed activation can leave the new program
durable without starting the service; reopen the folder to inspect its active
identity. The peer also raised the retained-snapshot total budget near the
maximum saved-state size; that boundary was not independently validated.

Automatic failure replay using retained compilation context remains U4 work.
Source-only check/test/replay do not reconstruct `UpgradeOld`; the guide says so.

## Final checks

- `bash ci/check.sh gcc clang drivers static`: `check.sh finished: pass`, no
  skipped checks. This includes the CLI/error goldens, all upgrade drivers,
  format-behavior audit, grammar, dependency rules and static checks.
- `bash ci/check.sh lean`: `check.sh finished: pass`, including migration lemmas,
  axiom checks and differential drivers.
- `make deps format-check tidy`: exit 0 after the review fixes.
- The earlier full sanitizer unit run passed. After the final review fixes,
  `test_upgrade_store` and `test_service_store` passed under ASan/UBSan, as did
  `check_upgrade_context.py` and `check_upgrade_adoption.py` against the rebuilt
  sanitizer executable. The nonempty-tail migration regression passed in GCC
  and sanitizer builds.

The local integrated log is `build/check-u2-release.txt`; the separate Lean log
is `build/check-u2-lean.txt`. Build logs are untracked. The commands and bounded
results above are the durable record; no nightly or later-milestone checks are
claimed here.

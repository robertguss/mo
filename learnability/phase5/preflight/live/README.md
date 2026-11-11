# M4 live upgrade verification

This milestone adds live whole-program upgrades at a service message boundary:
MCP sessions and a GET event stream for `notifications/tools/list_changed`, the
`mo-upgrade-open`, `mo-upgrade-page` and `mo-upgrade-commit` tools behind a second
credential, the live admission path, the VM swap entry, `mo push`, and replay of a
failure from the program a folder retained for it. The operator-catalog polish and
the remote tunnel procedure remain M5; the drill, evaluation and release remain M6.

## Order of work

The streaming transport landed first, with its tests, before any swap code; the
capability advertises `listChanged: true` from that commit on. The VM boundary tests
(`tests/test_service_swap.c`) were written against the planned swap API and failed
to build (41 errors) before the entry existed. The store activation fault matrix and
the TLA+ swap model were written before the server's swap. The server-side upgrade
tool tests and the end-to-end driver came after the implementation; they are not
red-before-implementation evidence.

## Acceptance evidence

| Requirement | Where it is pinned |
| --- | --- |
| Sessions, one stream per session, 400/404/406, slow consumer, stream cap, shutdown | `tests/test_protocol_server.c` `streams`; `tests/runtime/http-transport-check.mo`; MCP 2025-11-25 transport semantics in `docs/design/upgrades.md` |
| Queued calls re-admitted in order under the new program; removed and retyped calls answered `removed_by_upgrade` only at finish; ticks of removed timers dropped and counted; timer phase kept by name; refusal leaves the old loop untouched | `tests/test_service_swap.c` |
| Every activation statement, BEGIN, and COMMIT before and after the real commit: the reopened folder is wholly pre- or post-upgrade | `tests/test_service_store.c` `activation_faults` |
| A rolled-back activation keeps the old program serving (E1425); an uncertain one stops the service with the store status | `tests/test_protocol_server_faults.c` `activation_outcomes` (compile-time renamed `mo_store_activate`; removing the stop fails it) |
| Credential refusals before any argument (E1414), no echo, service-token-only callers see no slot details, busy slot (E1415), stale base naming the active digest (E1416), size (E1417), page refusals naming both values (E1418), expiry, digest mismatch, already active, a lost-reply retry reported as already active | `tests/test_protocol_server.c` `upgrade_tools` |
| Client refuses a non-loopback URL before reading either credential; start refuses a short credential or one equal to the token; a push under concurrent callers counts every acknowledged call once; one notice per session stream after commit and none for refusals; refreshed tool list; timer phase; pre-upgrade events keep handler names; live-budget migration refused (E1424) while the old program serves; live migration; kill -9 after commit and folder-only restart; second serve exits 75; no credential in logs, outputs or folder bytes; kill between staging and commit restarts the old program; the checked restart accepts what the live budget refused; two upgrades with no message between; replay of a first-program failure without its source after two upgrades | `ci/check_upgrade_live.py` |
| Kills at random moments, including mid-push, with concurrent callers and old, new and invalid variants | `ci/check_kill_loop.py` |
| Ambiguous retained program (E1431) | `tests/test_service_store.c`; `mo replay` renders it |

## Model, proofs and fuzzing

`models/Swap.tla`: the passing instance has 29,266 distinct states at depth 20; each
negative control finds its named violation (`SingleAuthority` 200 states,
`MessageConservation` 792, `NoCrossUpgradeRecord` 1,197). The model has three
programs, three messages and at most two activations; client retries are not
modeled. See `models/README.md`.

CBMC (CBMC 5.95.1) proves `mo_upload_base64` and `mo_upload_page` at a reduced page
bound of 4 decoded bytes and a 10-byte bundle, and the reach checks reach every block
of both (rerun at `dbc606d1`: proofs 11 s and 74 s, reach 10 s and 16 s);
`cbmc/README.md` records the bounds. Four deliberately broken readers each failed
the proof.

Fuzzing, sanitizer-enabled libFuzzer targets at commit `948fbda1` (no front-end
source changed afterwards):

| Target | Seed | Seconds | Executions |
| --- | ---: | ---: | ---: |
| `fuzz_check` | 4011108 | 300 | 179,256 |
| `fuzz_lower` | 4011109 | 300 | 7,616 |
| `fuzz_codegen` | 4011110 | 300 | 1,096,884 |
| `fuzz_program` | 4011111 | 300 | 6,380 |
| `fuzz_upgrade_pages` | 4011112 | 60 | 282,302 |
| `fuzz_http` | 4011112 | 60 | 1,302 |
| `fuzz_mcp_views` | 4011112 | 60 | 10,951 |
| `fuzz_protocol_server` | 4011112 | 60 | 25 |
| `fuzz_mcp_rpc` | 4011112 | 60 | 223 (after the oracle fix below) |

`make CC=clang fuzz-smoke FUZZ_SECONDS=10` at `dbc606d1` ran all 42 targets and
passed. These are finite observations, not proofs over all inputs.

Kill loop: `python3 ci/check_kill_loop.py MO --seed 20261108 --kills 1000` passed in
2,467 seconds: 62 kills landed mid-commit, 279 pushes, 98 activations, 81 invalid
pushes refused, 109 pushes killed mid-flight, 15,801 calls (673 with unknown
outcome, all reconciled). It ran against the build of commit `86fa9b06`; later commits
changed the server's refusal wording, the retry order of the open checks and the
client's failure output. A second run, `--seed 20261109 --kills 200`, against the build
of `a6a923df` passed in 592 seconds: 14 kills mid-commit, 48 pushes, 12 activations,
18 invalid pushes refused, 19 pushes killed mid-flight, 3,443 calls. The later
active-program copy fix (`cd2a1c23`) changes only an allocation the task queue's
small bundle never overflowed. Neither run establishes power-loss durability.

## Failures kept and corrected

- The first driver run found that the previous image was released as soon as no
  request borrowed it, so events from before the upgrade rendered
  `<program not retained>`. Events borrow it too: the previous program now stays
  until the next upgrade, which waits until no earlier request holds it.
- Four driver threads calling at full speed exhausted the shared 20-per-second rate
  limit, and the push itself was rate limited. The limit is shared by design; the
  driver now calls at about ten per second. A service-token holder can still starve
  pushes; the checked restart is the recovery path.
- The first slow-consumer unit test was ineffective: the stream head had already been
  written before the peer stopped reading. The test now starts the peer unwritable.
- Assumed: a stopped service could be pushed to and refused with E1428. `mo serve`
  exits when its supervisor stops the service, so that path is unreachable from the
  network; E1428 keeps a reachability explanation in `tests/errors/README.md`.
- Assumed: a comment-only change keeps the image key, giving two retained programs
  with distinct sources and one key. Any source byte changed the key in this build,
  so E1431's ambiguity is pinned at the store level instead.
- The plan's ten queued calls cannot accumulate through HTTP, because the server
  accepts one connection per poll and runs one message per poll. Re-admission order is
  proven at the VM level; the driver checks conservation under concurrent callers.
- Two parallel worker worktrees were cut from `main` instead of the branch tip; both
  workers stopped on the base check and were re-pointed before editing.
- The 60-second `fuzz_mcp_rpc` run stopped on its first seed: its oracle expected
  `initialize` to yield a plain reply after it began yielding the session action. The
  oracle now accepts the session reply.
- The first full driver run after the review found that a service with 32 or more
  tools failed to start ("the server ran out of memory"): the server copied the
  active bundle and its context in two allocations from an arena sized only for
  their sum. The copy is one allocation now; the CLI driver's paged-catalog service
  is the regression case.
- The `e1409-token` error case leaves a `service.sqlite` in its folder because the
  checked-start path opens the store before reading the token. This is M2 behavior,
  not changed here; the stray file was deleted.

## Independent review

The completed Compound Engineering review (`20261108-165644-6395ea33`, full depth)
reported eight actionable findings. Seven were fixed: the guide claim for `mo push`,
the missing server-side test for refused and uncertain activations, a stale
upgrade-record count in the kill loop, `mo push --json` output on client failures,
the wording when a push fails before its commit, refusal messages without both
values, and a migration name visible to service-token-only callers. The eighth, that
retyped queued arguments could be reinterpreted, was rejected: the saved encoding
carries its schema, and a new swap test shows an Int argument retyped to Float is
answered as removed. Two residual items were also fixed: a repeated push after a lost
reply is reported as already active, and a waiting commit's boundary allowance starts
when compilation ends. Remaining residual risks are listed in the pull request.

## Verification limits

- No measurement of the live pause on the task queue's 5,000 jobs with 2,048-byte
  payloads was made in this milestone; the drill owns that evidence.
- Native encode and decode work during queue re-admission is not charged to the live
  migration budget.
- Candidate initialization runs on the serving thread under the checked-restart work
  bound before the boundary; a hostile initializer can stall callers within that bound.
- The compiler child shares the service's identity and secrets; it is not an OS sandbox.
- A service killed during a compile can leave the child's `/tmp/mo-upgrade-*` scratch
  directory.

## Final checks

- `MO_ALL_DRIVERS=1 bash ci/check.sh gcc clang sanitize drivers static models` at
  `cd2a1c23`: `check.sh finished: pass`. The drivers ran against the gcc debug, clang
  debug and clang sanitizer executables, including `ci/check_upgrade_live.py` and the
  short kill loop with upgrade steps; the models group ran the swap model and its
  three controls. The log is the untracked `build/check-final.txt`.
- An earlier combined run failed on driver lists that pinned nine operator tools and
  on the command list in `ci/check_cli.py`; a later one failed on the 32-tool start
  defect above. Both were fixed before this run.
- Not run: `bash ci/check.sh lean` and `bench` (no Lean or benchmark input changed),
  and no nightly is claimed.

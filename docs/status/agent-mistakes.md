# Agent mistakes

The ledger the `correct` skill keeps: each kind of mistake agents have repeated in this repo, the
evidence, and what now stops it. A class is listed once it has happened twice. Levels, strongest
first: structure, compiler, CI check, test, prose. A class at `prose only` that recurs moves up a
level in the PR that fixes the recurrence.

Last run: 2026-10-18, PRs #80 to #121 and the project threads. GitHub held no review comments for
this range, so the evidence is fix-up commits inside PRs, the Review sections of PR bodies, red
`check` runs, and corrections in the project threads. Class 1 bumped 2026-10-24 for #178 and
#188's gap-review follow-up. Classes 6 and 7 bumped 2026-10-24 for MO-168's fresh-context review;
class 6 also bumped for MO-152's nightly workflow review.

| # | Class | Evidence | Enforced by | State |
| --- | --- | --- | --- | --- |
| 1 | Docs that describe a changed behaviour left stale: `sample-blockers.md`, `architecture.md`, `bytecode.md`, `agent-guide.md`, comments, usage strings | 19 times: #81, #85, #94, #95, #101, #105, #107, #110, #111, #113 (2), #115 (3), #116, #119, #139 (`AGENTS.md`), #178 (`examples/README.md`, tools hold-back, CBMC inventories, `check.h`), #188 (unsupported gap descriptions after main advanced) | `guide_claims` in `ci/check_cli.py` for the agent guide (#115); `ci/check_conformance.py` rejects unmapped reference blocks and unreviewed reference changes (#188), not stale implementation gaps; `review-change/rules.md` for the rest | check-enforced for guide claims and reference mappings; prose only for implementation gaps and other docs |
| 2 | Library functions built as native signatures with hand-built Core where `docs/design/library.md` says they are written in Mo in `lib/` (`List.map`, `Map.update`, `List.sort_by_desc`, `List.count`, `Option.or_fail`) | #86, #87, #88, #97; moved into `lib/` by MO-46 part 2 (#121) | nothing: `make library-map` covers only `prims.def` and `hostops.def` | open: nothing stops a new one |
| 3 | Lean-dependent drivers skipped locally (no elan in cloud sessions), so hosted CI found the failures, or the PR merged without them | #97 (merged without Lean), #116 (2 red runs), #121 (2 red runs, body said drivers passed); nearly every PR body since #100 says Lean was skipped | AGENTS.md "a skip is not a pass" | prose only (hosted CI removed 22 Oct); cloud sessions don't run `.agents/setup` |
| 4 | PRs green alone merged onto a main that had moved, turning main red | #118 + #119 + #120: `tests/errors` pins E1003 at `main.mo:4097`, `lib/list.mo` takes one source slot, main red from f4e7f03 until #121; conflicts on the lowering table, `ci/check_cli.py` and `sample-blockers.md` in every sample PR after #106 | nothing (hosted CI removed 22 Oct) | open |
| 5 | The fresh-context review step skipped | #116, #117, #118, #121 (and #97 skipped a gate) | PR template Review section; `ship-task` | prose only |
| 6 | Tests or harnesses that pass without testing anything: loose exit-code matching, a count of 0 allowed, no negative control, surviving mutants, an empty fixture, a stale assertion, a CBMC specification that checks what a refusal leaves but not what an acceptance requires | 15 times across 11 tasks: #82, #87, #110, #113, #114, #115, #116, #138 (3 mutants survived), #139 (3 mutants survived), MO-168 (unterminated output and output-without-main mutants survived), MO-152 (scratch workflow check missed a mismatched artifact path) | `review-change` mutant step; `test-change` "see it red"; `ci/test_reference_examples.py` in `drivers` rejects both MO-168 mutants; nightly comparison and artifact upload share the job's `MO_CORE_DIFF_OUT`, removing the duplicated path | test-enforced for MO-168 output boundaries; structure-enforced for the nightly output path; prose only for other harnesses |
| 7 | Logic bugs in new checker, lowering or tools code that only a fresh review caught | 9 times: #81 (2), #85, #86, #113, #114 (2), MO-168 (declaration whitespace and declarations inside text) | `review-change`; `ci/test_reference_examples.py` in `drivers` checks whitespace variants and ignores declaration-like text | test-enforced for MO-168 declaration detection; prose only elsewhere |
| 8 | Checker loops not charged to the work budget, or a size without a named limit | #81 (2), #86 (`src/check/bodies.c` missing `work(p)`) | AGENTS.md "Every input has a named limit"; `review-change/rules.md` | prose only |
| 9 | Static drift: clang-format, a module-boundary include, an unchecked result | #81, #85, #111, #121 | `static` group: `make format-check`, `make deps`, `make tidy` | prose only: `static` runs only by hand since hosted CI was removed (22 Oct); format-check needs clang-format 14.0.6 |
| 10 | Boilerplate or wrong verification evidence pasted into PR bodies (Lean 4.29.0, Z3, Valgrind, a build path that doesn't exist) | #84, #89, #90, #91, #92, #95, #96, #98 | nothing (the required hosted `check` was removed 22 Oct) | prose only |
| 11 | Python caches committed | #102, #104, #105, #106 | `.gitignore` (#112) | enforced |

On 22 Oct Robert removed hosted CI (no workflows, no required `check`), so rows 3, 4, 9 and 10 now
depend on agents running `ci/check.sh` groups locally.

MO-152 restores nightly checks of main, including Lean and static checks; it does not restore a
PR gate. Its review also corrected a one-off claim that GitHub concurrency queues every request:
the active run is preserved, but only the newest pending run is retained.

One-offs this run: macOS build breaks (#81; tests that create non-UTF-8 file names, which APFS
refuses, found by a local push on 22 Oct), a skill written into `.claude/skills/` instead of
`.agents/skills/` (#108), a Python line over 100 columns (#115).

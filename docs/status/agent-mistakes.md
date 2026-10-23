# Agent mistakes

The ledger the `correct` skill keeps: each kind of mistake agents have repeated in this repo, the
evidence, and what now stops it. A class is listed once it has happened twice. Levels, strongest
first: structure, compiler, CI check, test, prose. A class at `prose only` that recurs moves up a
level in the PR that fixes the recurrence.

Last run: 2026-10-18, PRs #80 to #121 and the project threads. GitHub held no review comments for
this range, so the evidence is fix-up commits inside PRs, the Review sections of PR bodies, red
`check` runs, and corrections in the project threads.

| # | Class | Evidence | Enforced by | State |
| --- | --- | --- | --- | --- |
| 1 | Docs that describe a changed behaviour left stale: `sample-blockers.md`, `architecture.md`, `bytecode.md`, `agent-guide.md`, comments, usage strings | 17 times: #81, #85, #94, #95, #101, #105, #107, #110, #111, #113 (2), #115 (3), #116, #119, #139 (`AGENTS.md`) | `guide_claims` in `ci/check_cli.py` for the agent guide (#115); `review-change/rules.md` for the rest | prose only, except the guide |
| 2 | Library functions built as native signatures with hand-built Core where `docs/design/library.md` says they are written in Mo in `lib/` (`List.map`, `Map.update`, `List.sort_by_desc`, `List.count`, `Option.or_fail`) | #86, #87, #88, #97; moved into `lib/` by MO-46 part 2 (#121) | nothing: `make library-map` covers only `prims.def` and `hostops.def` | open: nothing stops a new one |
| 3 | Lean-dependent drivers skipped locally (no elan in cloud sessions), so hosted CI found the failures, or the PR merged without them | #97 (merged without Lean), #116 (2 red runs), #121 (2 red runs, body said drivers passed); nearly every PR body since #100 says Lean was skipped | hosted `check` with `CI=1`; AGENTS.md "a skip is not a pass" | open: cloud sessions don't run `.agents/setup` |
| 4 | PRs green alone merged onto a main that had moved, turning main red | #118 + #119 + #120: `tests/errors` pins E1003 at `main.mo:4097`, `lib/list.mo` takes one source slot, main red from f4e7f03 until #121; conflicts on the lowering table, `ci/check_cli.py` and `sample-blockers.md` in every sample PR after #106 | nothing: the ruleset requires `check`, not an up-to-date branch | open |
| 5 | The fresh-context review step skipped | #116, #117, #118, #121 (and #97 skipped a gate) | PR template Review section; `ship-task` | prose only |
| 6 | Tests or harnesses that pass without testing anything: loose exit-code matching, a count of 0 allowed, no negative control, surviving mutants, an empty fixture, a stale assertion, a CBMC specification that checks what a refusal leaves but not what an acceptance requires | 12 times across 9 PRs: #82, #87, #110, #113, #114, #115, #116, #138 (3 mutants survived), #139 (3 mutants survived) | `review-change` mutant step; `test-change` "see it red" | prose only |
| 7 | Logic bugs in new checker, lowering or tools code that only a fresh review caught | 7 times: #81 (2), #85, #86, #113, #114 (2) | `review-change` | prose only (works when it runs) |
| 8 | Checker loops not charged to the work budget, or a size without a named limit | #81 (2), #86 (`src/check/bodies.c` missing `work(p)`) | AGENTS.md "Every input has a named limit"; `review-change/rules.md` | prose only |
| 9 | Static drift: clang-format, a module-boundary include, an unchecked result | #81, #85, #111, #121 | `static` group: `make format-check`, `make deps`, `make tidy` | enforced; cloud format-check needs clang-format 14.0.6 |
| 10 | Boilerplate or wrong verification evidence pasted into PR bodies (Lean 4.29.0, Z3, Valgrind, a build path that doesn't exist) | #84, #89, #90, #91, #92, #95, #96, #98 | the required hosted `check` status (#100) | enforced |
| 11 | Python caches committed | #102, #104, #105, #106 | `.gitignore` (#112) | enforced |

One-offs this run: macOS build breaks (#81; tests that create non-UTF-8 file names, which APFS
refuses, found by a local push on 22 Oct), a skill written into `.claude/skills/` instead of
`.agents/skills/` (#108), a Python line over 100 columns (#115).

# I1 verification

I1 reconciles the accepted private Phase 2 checkpoint with main using a merge
commit. The single draft PR is [#81](https://github.com/robertguss/mo/pull/81).
Do not merge it: Robert approves merges (D241).

## Inputs and decision

- Main: `1c28e8bf9ef98a7c06a87be6f8617958077b5793`.
- Private: `cf0f73bc2477dbc11e13f2a4630e90f6006ddfeb`.
- Merge base: `3523c4af72188a86a2314c3db4fc14b3668c096d`.
- Full history, not shallow: 22 commits unique to main, 136 unique to private.
  All supplied revisions matched before resolution; 15 conflicted files matched.
- Existing checkouts were untouched. A dedicated clone and detached parent
  worktrees live under the task workspace.
- Robert approved preserving main’s `compare`/`to_text` reservation and E0082
  for unrelated helpers: “Yes proceed with that” (2026-10-15). The checker,
  diagnostic golden and architecture retain that rule. Private helper-name
  fixtures now assert E0082; generic/import/lowering fixtures use ordinary
  helper names without changing the behavior they test.

Main’s abilities/generics, SCC recursion check and Number literal correction are
retained. The private checkpoint supplies imports, constants/initializers,
examples/tests/anchors, scalar/structural promises, captures, reports and
capability entry. Independently created headers and recursion implementation
were compared and reconciled; duplicate auto-merged declarations were removed.
No numeric code has different identifiers between the parent registries.
No error-code renumbering was necessary.

## Untouched parent baseline

Following `.agents/skills/verify-mo/SKILL.md`, each parent was built and driven
before conflict resolution. On the Mac:

- `make test CC=clang`: exits 2 on both at an existing mixed-enum conditional
  warning in `src/bytecode/assemble.c`. Homebrew Clang 22 reproduces it.
- GCC 16’s unmodified build also fails because Darwin’s `mkdtemp` declaration
  is hidden by the selected feature macros.
- To obtain executable baselines without editing either parent:
  `make -j6 test CC='clang -Wno-error=enum-compare-conditional -D_DARWIN_C_SOURCE'`.
  Both build, then exit 2 at the pre-existing library path assertion in
  `test_check` (main line 513, private line 1723).
- Every debug test was then run independently: main 33/35 suites pass, private
  34/36 pass. Both fail `test_check` and abort at sandboxed Unix socket setup in
  `test_host`. Outside the sandbox, the host test gets past the socket and
  reaches an invalid UTF-8 filename that the host filesystem rejects
  with errno 92, Illegal byte sequence (independently reproduced outside the sandbox). These failures are not
  attributed to the merge.
- Both parent program/lowering/VM suites pass. Private’s debug deadline suite
  passes. Neither parent Doctor is claimed green.

Private greet, unchanged: check exits 0; run exits 0 and prints `Hello, Ada!`;
test exits 0 with `3 passed, 0 failed, 0 stopped`. Main check exits 1/E1103;
run and test exit 1/E1400. Every sample JSON totals record has `dropped: 0`.
The complete before/after table is in [sample-blockers.md](sample-blockers.md).

## Recoverable platform issues

The merged tree fixes both enum warnings with explicit `uint32_t` casts and
adds Darwin’s declaration macro to the build flags. The path fixture compares
against its canonical library path, preserving the loader’s existing behavior
on `/tmp` versus `/private/tmp`. No warning, sanitizer or assertion is disabled.
Merged Mac `make test CC=clang` now passes through checking and lowering-related
suites, then reaches the same host socket/filesystem restrictions.

A dedicated Ubuntu 24.04 AArch64 container on the connected Mac runs the full
host suite on a Linux filesystem. It uses GCC, Clang 18, clang-tidy, Bison 3.8.2,
CI’s documented clang-format 14, Python and pinned Lean 4.34.0. All 36 merged
Clang debug suites pass there.

The first full Linux gate passed GCC/Clang tests but sanitizer linking exposed
Ubuntu AArch64 Clang’s missing `__muloti4` when using the default GCC runtime.
`nm` confirmed the symbol in Clang’s installed builtins archive. The complete
gate is rerun with that archive added to normal linkage; checks are unchanged:

```sh
runtime=$(clang --rtlib=compiler-rt --print-libgcc-file-name)
export MAKEFLAGS="-j6 LDLIBS=-lm\\ $runtime"
CI=1 bash ci/check.sh
```

Full gate status: pending final rerun. GCC/Clang/ASan tests, CLI, Unicode,
every fuzz target, grammar and Lean model/protocol checks passed. The first
configured gate then stopped at Core fixture comparison because the initial
Mac archive transfer left AppleDouble `._*` metadata files in the container.
Their AppleDouble magic was verified before removing only those transfer
artifacts; repository fixtures were retained. The complete gate is rerun from
the beginning before this PR can be considered verified.

## Focused merged behavior

Executed on the Mac’s merged `build/clang/debug/mo`; raw command/exit/output
records and the source fixtures are preserved in the task’s `evidence/` folder.

| Requirement | Observed result |
| --- | --- |
| Unchanged greet checks/runs/tests | exits 0; `Hello, Ada!`; 3 passed, 0 failed, 0 stopped |
| Two-module import plus same/cross-module constants | main imports `Origin.base`, adds local constant, prints `20`; exit 0 |
| Cyclic constants | `mo check --json` exits 1 with E0089, dropped 0 |
| Captures, try, Option or, interpolation, constant and specs lower/run | combined probe checks, prints `9`, and passes both example/test observations |
| Main accepts Args, Console, Files, Clock, Random and Env | six typed parameters are supplied; run with Ada prints `Ada`, exit 0 |
| Examples and promise blocks check | combined spec probe and scalar promise check exit 0 |
| Scalar promise execution | `mo test` exits 0; 1 passed, 0 failed, 0 stopped |
| False List(Int) promise shrinks | `mo test --json` exits 1; input/result `[0]`, 3 calls, 1 failed, 0 stopped, 0 errors |

The capability probe proves parameter delivery, not support for every operation:
for example Clock.now lowering remains staged at E1150. No new operation was
added by the reconciliation. The false-list test’s `[0]` is a minimal nonempty
counterexample to the intentionally false “always empty” promise.

## Blast radius and scope

[i1-blast-radius.md](i1-blast-radius.md) records each reconciled function,
callers, affected contracts, shared/reused-handle chains and executed coverage.
Its safety claims are tied to real suites and the final gate, not compilation.

No tasks C1, F5, LW5 or M1 were started. Examples and the Language Reference
are unchanged. No squash or rebase is used. The PR is not merged.

## Raw evidence locations

All paths are relative to the dedicated task workspace, outside the build tree:

- `evidence/main-clean-baseline.log`, `private-portable-baseline.log`.
- `evidence/parent-suite-results.json`, parent sample JSON and customizer probe.
- `evidence/merged-mac-test.log`, `linux-merged-unit.log`.
- `evidence/linux-full-ci.log` (runtime-link failure) and
  `linux-full-ci-runtime.log` (configured full gate).
- `evidence/merged-samples.json`, `acceptance-results.json` and
  `acceptance/*/main.mo` source fixtures.
- `evidence/blast-radius-callsites.txt`, function inventory and registry comparison.

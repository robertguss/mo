# AGENTS.md

Guidance for coding agents (Claude Code, Codex, Cursor and others) working in this repository.
`CLAUDE.md` imports this file.

## What this is

Mo is a pre-release language and runtime for "living software", designed for agents first. The
toolchain is one C17 program, `mo` (`check`, `run`, `test`, `format`, `fix`), that compiles a whole
program from source to bytecode and runs it on its own VM. Decisions are cited as `D2xx` and work
items as `MO-NN` (commit titles start with the ticket, e.g. `MO-83: add List.all? ...`).

The documents that define correct behaviour, in order of authority:

- `docs/reference/README.md`: the Mo Language Reference v0.1. It is embedded into the binary
  (`mo guide --reference`) by `ci/embed_docs.py`, as is `docs/agent-guide.md`.
- `docs/design/architecture.md`: how the toolchain is built (sections 2, module boundaries, and
  11, testing strategy, are the ones to know). `docs/design/bytecode.md` is the bytecode spec.
- `docs/design/implementation.md`: the five conditions every C change must meet.
- `docs/design/library.md`: where each library function is implemented.
- `docs/status/sample-blockers.md`: what still stops each of the twelve `examples/*.mo` from passing.

## Commands

Requires a C17 compiler, POSIX `make` and Python 3. `.agents/setup` installs the full toolchain
(gcc, clang, libFuzzer, clang-format, clang-tidy, bison, CBMC, and Lean via elan in `/opt/elan`).

```sh
make                       # build build/<compiler>/debug/mo (CC=cc by default -> build/cc/)
make test CC=gcc           # build and run every unit test (each tests/test_*.c is its own program)
make sanitize CC=clang     # the same tests under ASan + UBSan
make -j8 build/gcc/debug/tests/test_check.test.run CC=gcc   # build and run ONE test file
make grammar               # executable grammar (bison) over grammar/tests, examples/, the reference
make deps                  # module include rules (table at the top of ci/deps.sh)
make library-map           # docs/design/library.md vs src/base/prims.def and hostops.def
make format / format-check # clang-format; use clang-format 14.0.6, newer versions can differ
make tidy                  # clang-tidy cert-err33-c (unused stdlib results)
make fuzz-smoke            # every fuzz target briefly (needs clang + libFuzzer; FUZZ_SECONDS=5)
make formal                # build Mo Core in Lean and check every proof (needs elan)
make cbmc                  # prove the bytecode reader, arena, verifier, primitives, path containment and resolution, refcounts, hash tables (needs CBMC)
bash ci/check.sh [group…]  # every check; groups: gcc clang sanitize fuzz drivers lean static cbmc
```

- Each compiler builds into its own `build/<compiler>/` tree, so switching `CC` doesn't rebuild.
  Changing the Makefile, or adding or removing a file in `src/`, rebuilds everything.
- Warnings are errors (`-Wconversion`, `-Wsign-conversion`, `-Wshadow`, `-Wswitch-enum`, …), and
  gcc and clang warn about different things, so a change to C code should build with both.
- Locally, `ci/check.sh` skips checks whose tool is missing and says so in its summary; with `CI=1`
  a missing tool fails. A skip is not a pass.
- The `drivers` group runs Python black-box checks against the built executables
  (`ci/check_cli.py` for CLI text/JSON goldens, `ci/check_golden.py`, `ci/check_unicode16.py`, …);
  the `lean` group builds Mo Core in Lean and compares the executables with it
  (`ci/check_lower_source.py`, `ci/check_core_differential.py`, …). CLI behaviour changes usually
  need an edit to `ci/check_cli.py` or the goldens under `tests/cli/`.

## Checks and merging

No Mo check runs on GitHub: there are no Actions workflows, and `main` requires no status check
(only force-pushes are blocked). The Socket Security app may still post its own checks; none is
required. Before opening a PR, run `bash ci/check.sh gcc` (build and unit tests)
and report its result in the PR body, listing any check it skipped for a missing tool (a skip is
not a pass; `CI=1` makes it fail instead). Run any other group the change reaches and say which
groups you did not run: usually `clang` and `static` (formatting, module includes, library map)
for C changes, `drivers` for CLI or golden changes, `static` for `prims.def` or `hostops.def`.
The slow groups (`cbmc`, `lean`, `sanitize`, `fuzz`) run only when someone runs them by hand. The
build embeds or checks `docs/agent-guide.md`, `docs/reference/README.md` and
`docs/design/library.md`, so after editing one, also run `bash ci/check.sh drivers static`. A PR
merges when Robert says so. He lets agents merge docs-only PRs without review: PRs that change only
Markdown files, other than those three. PRs are squash-merged.

## Architecture

Pipeline: source → parser (`syntax`) → names, types and rule checks (`check`) → lowering to Mo
Core and monomorphization (`lower`) → Core checker (`core`) → bytecode compiler with reference
counting, borrowing and in-place reuse (`codegen`) → loader and verifier (`bytecode`) → VM (`vm`).
The VM never sees source, syntax trees or Core.

Each directory in `src/` is a module with one public header named after it (`check/check.h`).
Headers are included by their path from `src/` (`"base/base.h"`). Allowed dependencies, enforced
by `make deps`:

| Module | May use (plus `base`) |
| --- | --- |
| `syntax`, `core`, `heap` | nothing |
| `check` | `syntax` |
| `lower` | `check`, `core` |
| `codegen` | `core`, `bytecode`, `heap/types.h` |
| `bytecode` | `heap/types.h` |
| `prim`, `host` | `heap` |
| `vm` | `bytecode`, `heap`, `prim`, `host` |
| `tools` (the `mo` command, `src/tools/main.c`) | everything |

Four shared contracts; changing one means changing every side that reads it:

- **Mo Core**: `formal/` (Lean, the reference evaluator) and `src/core/core.h`, exchanged as text
  (`formal/TEXT.md`, `formal/TRANSLATION.md`). The C VM is differentially tested against Lean's
  evaluator; `tests/core/*.core` hold the samples and other fixtures as Core text.
- **Bytecode**: `docs/design/bytecode.md` and `src/bytecode/bytecode.h`.
- **Primitives**: `src/base/prims.def`, an X-macro table read by the checker, codegen, verifier and
  VM. Every line needs a row in `docs/design/library.md` (`make library-map`).
- **Capability operations**: `src/base/hostops.def`, the same four readers plus `src/host/`.
  All outside-world effects go through capabilities, which have real and fake implementations.

Adding a library function typically touches its `check` signature (`src/check/signatures.c`,
`constants.c`), lowering (`src/lower/expressions.c`), possibly a primitive in `prims.def` and
`src/prim/`, a `tests/lower-source/<name>/main.mo` fixture, `tests/test_program.c`, and
`docs/status/sample-blockers.md` when it unblocks a sample.

## C rules (Tiger Style, D207)

- `MO_ASSERT` is never compiled out; use it liberally.
- Every input has a named limit in `src/base/limits.h`. Compiler-input limits are diagnostics;
  runtime limits (stack, call depth, heap) are bug reports.
- Allocate from arenas. Only `base` and `heap` call `malloc` directly.
- Every result is checked: `MO_MUST_USE` on Mo's functions, `make tidy` for the standard library,
  and a `(void)` cast to discard on purpose.
- No silent numeric conversions; `-Wconversion` is on. 128-bit ints are `mo_i128`/`mo_u128`.
- Anything reading outside input gets a fuzz target in `fuzz/` with seeds in `fuzz/corpus/<target>/`.
- Fault injection uses compile-time renaming (`-Dcalloc=mo_vm_fault_calloc` etc.) on specific
  translation units in the Makefile, never runtime hooks. Follow that pattern for new fault tests.
- Style: `.clang-format` (LLVM base, 4-space indent, 100 columns).

## Tests

- `tests/test.h` is the whole harness: `CHECK(cond)` records a failure and continues;
  `mo_test_report(name)` sets the exit status. Tests go through public headers, not internals.
- Checker diagnostics are pinned by `tests/check/diagnostics/eNNNN` inputs with adjacent `.diag`
  expectations (format in that folder's `README.md`). Nothing regenerates them; edit by hand.
- Every code in `src/base/codes.h` needs a case in `tests/errors/` (exact `mo check` message, see its
  `README.md`) or a line in `tests/errors/unit.tsv`; `ci/check_errors.py` fails otherwise.
- `tests/golden/` is the golden corpus: whole programs with their exact stdout, stderr and exit status
  (format in `ci/check_golden.py`). Write the expected output by hand from the reference; a wrong-looking
  result is a bug to fix or list in `docs/status/golden-gaps.md`, never a new expectation.
- `docs/agent-guide.md` (`mo guide`) tells agents what this build does. A change to what a `mo`
  command does, or to which forms are unfinished, updates the guide in the same PR;
  `guide_claims` in `ci/check_cli.py` runs the guide's program and checks each claim it makes.
- `grammar/mo.y` is an instrument, not the compiler: the real parser is hand-written in
  `src/syntax/`, and a fuzz target checks both accept and reject the same inputs. Bison refuses any
  conflict, which keeps the grammar unambiguous.

## Workflow

Start any MO task with the `ship-task` skill; it runs the others in order: `plan-change`, then
`test-change`, then `review-change`, and `notion-sync` once the PR is open. Skills live in
`.agents/skills/`. The PR template asks for each step's output. A one-line fix can keep its plan to
a sentence; it still gets tests and a review.

After a PR opens or merges, or after Robert makes a decision, run `notion-sync`.

When Robert or a reviewer corrects a mistake, fix it and add or bump its row in
`docs/status/agent-mistakes.md` in the same PR. A row that recurs is fixed at a stronger level, using
the `correct` skill.

After a solved, verified problem, offer once to invoke the `ce-compound` skill at the completion checkpoint only when the work produced durable project reasoning that is not readily recoverable from the final code, tests, types, comments, or existing documentation, and losing it would plausibly cause recurrence, material risk, or substantial rediscovery. Apply this counterfactual: if the learning document disappeared, would a future engineer reading the final implementation still be likely to repeat the mistake or redo substantial investigation? If not, do not offer. Completion, effort, and diff size alone are not enough. Offer at the checkpoint so a qualifying learning can ship in the PR that produced it, and only where the repository treats captured learnings as tracked, committed knowledge.

Write every report, summary, or handoff to the user through the `ce-noslop` skill. This applies when you are the top-level agent writing to the user, not when you are a subagent reporting to its caller. Do not apply it to code, config, verbatim quotes, or text the user asked to post as written.

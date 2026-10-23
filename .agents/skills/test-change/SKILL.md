---
name: test-change
description: Test a Mo change. Use when writing or changing tests, choosing which test layer covers a behaviour, or when a test or golden goes red.
---

# Test a change

A good Mo test pins one behaviour to its contract: it goes red when that behaviour breaks and stays green through every refactor that keeps it. Warnings-as-errors, the sanitizers and the Lean evaluator already guard a lot, so spend tests on behaviour those can't see.

## Pick the layer

Each behaviour has one home. Pick by what the behaviour is:

| Behaviour | Where the test lives | Expected value comes from |
| --- | --- | --- |
| Logic in one C module | `tests/test_<module>.c`, through the module's public header | the header's contract and `docs/design/` |
| A checker diagnostic | `tests/check/diagnostics/`: a `.mo` input and its `.diag`, one diagnostic each (format in that folder's `README.md`) | the reference section the error code cites |
| A library function or lowering | `tests/lower-source/<name>/main.mo`, its outcome in `ci/check_lower_source.py`, its output in `ci/check_cli.py`, and a case in `tests/test_program.c` | the reference, and Lean for the outcome |
| CLI text or JSON | goldens under `tests/cli/<stage>/`, read by `ci/check_cli.py` | the reference and the agent guide |
| Core or VM semantics | Core text in `tests/core/`, run against the Lean evaluator by the `lean` group | the Lean model in `formal/` |
| Code that reads outside input | a fuzz target `fuzz/fuzz_<name>.c` with seeds in `fuzz/corpus/<name>/` | the invariant in architecture section 11 |
| Syntax | `grammar/tests/`, and the parser corpus in `tests/test_syntax_corpus.c` | the reference grammar |
| A sample program | `examples/*.mo`, and its row in `docs/status/sample-blockers.md` | the sample's own promises |

When a behaviour fits two rows, take the one closest to the user: a CLI golden over a unit test for anything `mo` prints.

## Steps

1. **Write one test and see it red.** For new behaviour, run it before the code exists. For existing code, break the behaviour on purpose, watch the test fail for that reason, then restore it. A test never seen red proves nothing. Done when you have watched it fail with the message you expected.

2. **Make it green on the tight loop.** Build and run just that test file (`make -j8 build/gcc/debug/tests/test_<name>.test.run CC=gcc`), or just the driver that reads the fixture. Then run it with clang, and run `make sanitize CC=clang`. Done when gcc, clang and the sanitizers all pass.

3. **Repeat per behaviour.** One test at a time, each seen red, until every behaviour in the plan has one. Done when the plan's test list is fully ticked.

4. **Run the gate.** `bash ci/check.sh gcc` at least, which every PR reports, plus `clang`, `static`, `drivers`, `sanitize`, `lean`, `fuzz` or `cbmc` when the change reaches what they check. Nothing runs them on GitHub. Read the summary: a skipped check is not a passed one. Done when it ends with all checks passed, or every skip is named in the PR.

## What a test asserts

- Exact values: the bytes, the outcome, the diagnostic's span and code. A count alone hides wrong contents.
- Limits from `src/base/limits.h` at the limit and one past it, plus empty input.
- The failure path of every `MO_MUST_USE` function the change adds.
- Behaviour through public headers, the `mo` command, or Core text. Internals stay free to change.

## When a test goes red

The code is wrong until shown otherwise. Fix the code to meet the contract. Change an expectation only when the contract itself changed, and name the decision or reference edit that changed it in the commit message. Goldens and `.diag` files are edited by hand, so read every changed line of their diff before committing.

## Reading a red gate

Groups run in order: gcc, clang, sanitize, drivers, fuzz, static. Fix the first red one; later failures are often its consequence. Rerun just that group (`bash ci/check.sh sanitize`); in a CI-orb run, read the end of its log.

- **gcc or clang**: a warning is an error, and the two compilers warn differently, so build with both.
- **sanitize**: rerun the single test file under `make sanitize` for the stack trace.
- **fuzz**: the crash input becomes a unit test or fixture first, seen red, then the fix.
- **drivers**: the script named in the step (`ci/check_cli.py`, `ci/check_lower_source.py`, …) prints the fixture and the expected and actual results.
- **static**: `make deps`, `make library-map` or `make format-check` names the file and the rule.
- **Another machine only** (the CI orb, a reviewer): a tool version differing from yours is the usual cause; `make format-check` expects clang-format 14.0.6, and newer versions format differently.

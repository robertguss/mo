---
name: write-tests
description: Write Mo C tests one tracer bullet at a time that pin real behaviour through public headers, never internals or values the compiler already guarantees.
---

# Write Tests (Mo)

Adapted from dzhng/skills `write-tests` (MIT) for Mo's C17 codebase.

A good test fails only when real behaviour breaks and passes through every refactor that preserves it. Mo's gates are warnings-as-errors (`-Wall -Wextra -Werror -Wconversion -Wsign-conversion`), sanitizers, and the Lean oracle — a test that re-asserts what those guarantee wastes budget.

## Workflow

1. **One tracer bullet at a time.** Write ONE test in `tests/test_<module>.c` using `CHECK(...)` from `tests/test.h`, watch it fail on the unfixed code for the intended reason, make the code earn green, then write the next. Never batch tests against imagined behaviour.
2. **Iterate on the fastest loop.** Build only that test: `make test CC=clang` (or `CC=gcc`), then `make sanitize` before handing off. Check the exit code, not just output. Run `bash ci/check.sh` only as the final gate.
3. **Prove it can fail.** Temporarily break the behaviour and confirm the test goes red for that reason, then restore. A test that cannot fail is coverage theatre.

## What to assert

- Observable behaviour through the public header (`src/<module>/*.h`, the `mo` CLI, or the Core text form), return values and bytes — never which internal helper ran. Isolated unit tests are for genuinely tricky pure logic: parsers, the heap/cell logic, state machines.
- Nothing the compiler already guarantees: no re-asserting types, field shapes, or rejected argument kinds (-Wconversion and the type checker fail first). Spend budget on arithmetic, branching, ordering, limits in `src/base/limits.h`, and side effects.
- Actual values, not counts alone: for dedup/normalise paths assert the stored canonical bytes, not just length == 1. The harness pattern in `tests/test_buffer.c` (`holds()` comparing bytes) is the model.
- The design contract, not current behaviour. If a test goes red, do not re-measure and pin the new number; that encodes bugs as baseline. Recalibrate only when the contract itself changed, and say so.
- Deterministic claims exact; noisy claims (fuzz, perf) as bands over seeds/runs, never a single lucky sample.

## Seams, mocks, limits

- Mo has no mock framework: test at real seams (arena, buffer, diag) with fixed inputs. Mock only at system edges (filesystem, clock, host ops in `src/base/hostops.def`).
- Respect explicit limits: exercise the boundary value and boundary+1, empty/zero input, and failure returns. Every Mo function whose result carries `MO_MUST_USE` must have its failure path tested.
- Keep tests deterministic, no timing or global-state dependence. If a bug only showed against the real path, fix the harness, not just the bug.

**Done when:** the new test fails-before/passes-after is demonstrated, `make sanitize` is clean, and no existing assertion was weakened without a contract change.

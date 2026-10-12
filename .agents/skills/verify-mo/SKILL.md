---
name: verify-mo
description: Drive the Mo toolchain the way a user does, build it, run the examples and gates, and capture evidence that proves the behaviour under test.
---

# Verify Mo

Generated for this repo following the `create-verification-skill` skill. Read cold: everything needed is here. Prove behaviour against the real artifact — a green compile alone is not proof.

## Launch

Build from the repo root (builds are per-compiler, only changed files recompile):

```sh
make CC=clang            # or CC=gcc -> build/clang/debug/mo (build/cc/debug/mo for the default cc)
ls build/*/debug/mo      # the binary to drive
```

Ready means the binary exists and runs; check with the Doctor step, not by process existence.

## Doctor

```sh
make test CC=clang       # unit tests pass on a clean base: "all checks passed" per suite
```

If this fails on an untouched tree, report it as pre-existing and do not build verification claims on top of it.

## Drive

- **Unit behaviour:** `make test CC=gcc && make test CC=clang`, then `make sanitize` (AddressSanitizer + UndefinedBehaviorSanitizer).
- **Examples parse and behave:** `make grammar` (executable grammar over `grammar/tests/`, `examples/*.mo`, and the reference's code).
- **Core oracle:** `make formal` needs elan/Lean (toolchain pinned in `formal/lean-toolchain`); the C Core text form is checked by `tests/test_core_text.c` against `formal/TEXT.md`.
- **Module and doc invariants:** `make deps`, `make library-map`, `make format-check`, `make tidy` (needs clang-tidy).
- **Fuzz smoke:** `make fuzz-smoke` (needs clang with libFuzzer; 20s per target by default).
- **Everything:** `bash ci/check.sh` runs every check CI runs. Locally a missing tool is a named skip; never report skipped as passed.

## Evidence

Capture per claim: the exact command, its exit code, and the decisive output lines (test totals, sanitizer reports, grammar/formal verdicts). Verify side effects, not just exit 0 — e.g. files the CLI wrote, diagnostics emitted on bad input (exercise a malformed `.mo` file and a truncated Core text input, not only happy paths). Keep raw evidence outside the build tree or name its location in your report.

## Feature map (seed)

- **Build and run `mo`:** `make` produces the CLI; prove with Doctor above plus one real invocation of the built binary.
- **Parse Mo programs:** `make grammar` over `examples/` and reference snippets.
- **Core text round-trip:** `tests/test_core_text.c` plus `make formal` where Lean is available.
- **Reject bad input safely:** fuzz targets (`fuzz/fuzz_*.c`) under `make fuzz-smoke`; malformed input must produce diagnostics, never a sanitizer report.

## Cleanup

Remove only processes and scratch dirs you started. Evidence survives cleanup; build outputs under `build/` may stay. Never kill by process name.

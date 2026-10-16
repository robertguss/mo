# LW5 verification and approved acceptance criteria

LW5 starts from verified current main `bfcdedcd` (PR81 merged), in a dedicated
clone and branch `lw5/map-literals`. Existing workspaces and I1 container files
are preserved. The Language Reference and examples are unchanged.

## Implemented surface

- The checked graph accepts map literals and retains alternating key/value
  operands in written order. Existing contextual inference handles empty maps
  from record fields, parameters, results and later `set` uses.
- Lowering emits existing Core `MapOp.lit`, preserving evaluation order, captured
  snapshots and insertion order. Required `Map.get`, `Map.set` and `Map.size`
  route to existing Core operations, including namespace/function-value routes.
- A jump that ignores a Unit result evaluates its final expression; the new
  map iteration fixture caught and now covers a dropped final `Console.print`.
- Existing type-ability checks reject Float, function and capability keys with
  E0080. No new Hash implementation or runtime collection operation is added.
- E0094 (reference 3.8) diagnoses equal scalar literals and repeated bare names,
  with primary repeated-key and secondary first-key spans. Numeric spelling,
  radix, underscore, negative-zero, text/char escape and triple-text decoding
  are normalized. Canonicalization and stable sorting charge work/arena limits.
  Mutable-name reads separated by possible assignments to that binding are
  conservatively distinct; the compiler does not assume those reads have equal values.
- Different parameter names that become equal at run time retain the existing
  bug message and exit status 70. Lean's `repeatedKey` stop is mapped to that
  existing message in the differential harness.

## Approved acceptance adjustments

Robert approved both proposed LW5 adjustments in the follow-up conversation:
“I approve.” The approval answered the two recommendations together:

1. Keep compile-time duplicate-key detection bounded to equal normalized scalar
   literals and unchanged bare names for LW5. Constants, qualified names,
   Unit/case constructors, composite keys and expression folding require a
   separate decision; this follow-up adds no language support.
2. Accept the documented newly exposed downstream sample errors. All six former
   map diagnostics are cleared; the twelve-sample total is 14→24 with every
   `dropped` value zero. Expenses rises 2→4, inventory 2→5, job-queue 1→3 and
   json-query 1→4; other counts do not increase. Diagnostics are retained.

The original literal no-increase criterion is replaced by that approved sample
acceptance adjustment. `Map.update`, `sort_by_desc`, `List.map`, `Option.or_fail`,
Time arithmetic and unavailable FileError text remain outside LW5. See
[sample-blockers.md](sample-blockers.md) for the complete current diagnostic table.

LW5 is complete under these amended acceptance criteria. PR #82 remains draft
and unmerged; Robert’s approval of these adjustments does not authorize merging.

## Baseline and supported workflow

A clean archive of main is built at `/lw5-baseline` in a dedicated Ubuntu24.04
AArch64 container `mo-lw5-ci-20261016`, created from an I1 toolchain snapshot.
`make -j6 test CC=clang` exits 0 with all 36 unit suites passing. All twelve
baseline samples have `dropped: 0` and match the I1 counts.

The integrated tree is tested at `/lw5-work`. This follows the supported Linux
workflow documented in [i1-verification.md](i1-verification.md), preserving
native Mac host restrictions instead of weakening assertions. GCC13, Clang18,
clang-format14, clang-tidy18, Bison3.8.2, Python3.12 and pinned Lean4.34.0 are
installed. Ubuntu AArch64 Clang requires its existing compiler-rt builtins
archive during sanitizer linkage:

```sh
runtime=$(clang --rtlib=compiler-rt --print-libgcc-file-name)
export MAKEFLAGS="-j6 LDLIBS=-lm\\ $runtime"
CI=1 bash ci/check.sh
```

No checks, warnings, assertions or sanitizers are disabled. The first full gate
caught the obsolete E1103 map refusal in `test_program`; it is replaced with a
successful checked-graph test asserting alternating Text/Int operands.

Full gate final result: **PASS**, exit 0, no skips. The complete rerun ends:

```text
==> All checks passed
```

- All 36 unit suites pass in GCC debug, Clang debug and ASan/UBSan builds.
- Full CLI, bytecode-text and Unicode checks pass in all three builds.
- All 23 fuzz runs pass at five seconds each, including the large-bytecode variant.
- Grammar, pinned Lean model/proofs/protocol and Core text fixtures pass.
- Core checker: 4,459 cases agree in each build. VM: 2,495 outcomes agree in
  each build, zero unsupported fixtures or unavailable dispatch cases.
- Lowering: 189 expected outcomes agree with Lean/VM. Lowered source: 72
  ordinary outcomes, 21 modules/27 outcomes and 19 spec programs/53 outcomes
  agree in each build, zero unexpected compiler/host refusals. Four explicitly
  deferred global/native refusals are checked separately.
- Primitive models: 54,251 calls per build agree; host model: 491 outcomes over
  33 operations per build agree. Code generation checks pass.
- Dependencies, library map, format, all 172 clang-tidy translation units and
  source-formatter fixed points pass.
- The post-gate 591-file input manifest has zero mismatches with the Mac tree.

The full gate verifies implementation commit `69d96b69a95a60c39982cb6c48c889b80faf2550`
and all its tests. The approval follow-up changes only this verification record,
the sample status and architecture prose. All 591 source/test/oracle/build inputs
still match the passing gate’s SHA-256 manifest, so no functional recheck is
required. The documentation diff is checked with `git diff --check`; the prior
full-gate result is not represented as a fresh run on the documentation commit.

## Evidence and blast radius

Native Mac Clang focused checker, checker-body, lowering and map CLI checks
pass. The shared source fixtures exercise contextual empty maps, readback,
insertion/evaluation order, mutable-key snapshots, captured function values,
first-class native routes and the runtime duplicate bug. These same programs
are in `ci/check_lower_source.py` for Lean/VM comparison.

Affected contracts are private checker helpers, the stable diagnostic registry,
normalized graph operands and existing Core map signatures. No Core/Lean
syntax, primitive ABI, runtime or public language rule changes. Checker callers
are the inference/program/spec/constant pipelines; lowering callers reuse the
ordinary argument continuations and source/native function adapters. The reused
handle chain is a map preserved across `set`, with the new map re-stored under
its variable and the original map read afterward in `map-native-routes`.

Raw evidence lives outside the build tree in this task’s `evidence/` directory:

- `linux-baseline-doctor.log`, `linux-baseline-samples.json` and
  `linux-baseline-environment.txt`.
- `mac-map-acceptance.json`, `mac-map-cli.log`, native focused suite logs,
  `linux-lw5-samples.json` and `linux-lw5-sample-comparison.json`.
- `lw5-input-manifest.json` and `linux-lw5-manifest-check.json`: 591
  source/test/oracle/build inputs match by SHA-256, zero mismatches.
- `linux-lw5-full-ci-attempt1.log` and the final full-gate log.

Both acceptance adjustments are approved. There are no remaining LW5 blockers
under the amended criteria. The draft PR is not merged.

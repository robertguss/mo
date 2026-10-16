# I1 blast-radius review

Reviewed with `.agents/skills/blast-radius/SKILL.md`. Every reconciled production
function is listed below; header-only conflicts are covered by their producers
and all public callers. Callers and symbol uses (including qsort callbacks) were
searched with `rg` across src/tests/fuzz/ci; the exact commands and results are
in the task’s `evidence/blast-radius-callsites.txt`.

No new module boundary or primitive/host table row is introduced by reconciliation.
Main’s abilities/generics and Number literal correction are retained. The private
checkpoint supplies the extended constants/specs/import/capture/entry graphs.
`native.h` is identical between parents except its explanatory comment;
`signatures.h` adds import/capture bookkeeping to main’s nominal/customizer index;
`recursion.c` adds promise-body edges to main’s SCC implementation. E0082 retains
main’s golden and meaning. Numeric code identities match across both registries.

## Function coverage

The merged Linux `make -j6 test CC=clang` passes every debug suite (36/36).
Checker rows use test_check/test_check_bodies/test_program/test_lower/test_cli;
bytecode rows use test_bytecode_io/test_bytecode_text/test_bytecode_verify/test_vm.
The full sanitizer/fuzz/grammar/Lean/dependency/library-map gate is recorded in
[i1-verification.md](i1-verification.md), separately from this debug evidence.

| Function | Affected contract reviewed | Executed evidence |
| --- | --- | --- |
| `src/check/bodies.c` / `data_type` | Nominal availability and field lookup keep main’s indexed lookup. | checker/lowering suites: pass |
| `src/check/bodies.c` / `native_identifier` | Spec-only operations stay lexical to specs and their lambdas. | checker/lowering suites: pass |
| `src/check/bodies.c` / `identifier` | Constant reads retain declaring identity and pause unresolved dependencies. | checker/lowering suites: pass |
| `src/check/bodies.c` / `member` | Owner/field/native dispatch keeps main’s Entry lookup by type. | checker/lowering suites: pass |
| `src/check/bodies.c` / `infer_node` | Prefix try joins the syntax dispatch without bypassing limits. | checker/lowering suites: pass |
| `src/check/bodies.c` / `infer` | try and Option forms join the existing bounded inference walk. | checker/lowering suites: pass |
| `src/check/bodies.c` / `failure_container` | Option/Result substitutions retain fresh scoped type variables. | checker/lowering suites: pass |
| `src/check/bodies.c` / `solve_failure` | Deferred defaults and propagation retain type and wrapper diagnostics. | checker/lowering suites: pass |
| `src/check/bodies.c` / `finish_dispatches` | Failure constraints reach the same bounded dispatch fixed point. | checker/lowering suites: pass |
| `src/check/bodies.c` / `anchor_inputs` | Names, duplicate/missing inputs and result bind in declaration order. | checker/lowering suites: pass |
| `src/check/bodies.c` / `infer_function` | Functions, constants, examples, promises and anchors share checked graphs. | checker/lowering suites: pass |
| `src/check/bodies.c` / `number_parameter` | Main’s Number literal bounds remain authoritative. | checker/lowering suites: pass |
| `src/check/bodies.c` / `literal` | Generic literals retain main’s range/type checks. | checker/lowering suites: pass |
| `src/check/program.c` / `check_imports` | Only selected written import routes are credited; unused routes report E0093. | checker/lowering suites: pass |
| `src/check/program.c` / `spec_source_order` | qsort callback preserves source order and remapped promise links. | checker/lowering suites: pass |
| `src/check/program.c` / `copy_text` | Original source spans are copied into the checking arena. | checker/lowering suites: pass |
| `src/check/program.c` / `hex_digit` | Decoded wording preserves Unicode and embedded NUL. | checker/lowering suites: pass |
| `src/check/program.c` / `wording` | Triple-quoted text and escapes retain decoded and original forms. | checker/lowering suites: pass |
| `src/check/program.c` / `check_specs` | Spec contexts, parameters, conditions and anchors remain typed before lowering. | checker/lowering suites: pass |
| `src/check/program.c` / `check_program` | Constants/specs/imports/entry compose with abilities and recursion certification. | checker/lowering suites: pass |
| `src/check/signatures.c` / `mo_use_import` | Signature and body lookup share bounded import-origin tracking. | checker/lowering suites: pass |
| `src/check/signatures.c` / `dependency_work` | All dependency traversals consume the shared program work limit. | checker/lowering suites: pass |
| `src/check/signatures.c` / `capture_dependencies` | Stored capability membership excludes phantom/function parameter types. | checker/lowering suites: pass |
| `src/check/signatures.c` / `watch_dependencies` | Closed recursive layouts can propagate capability membership. | checker/lowering suites: pass |
| `src/check/signatures.c` / `data_dependencies` | Hash/key/capture masks converge without expanding recursive layouts. | checker/lowering suites: pass |
| `src/check/signatures.c` / `customizer` | Approved main rule and E0082 reason are unchanged. | checker/lowering suites: pass |
| `src/check/signatures.c` / `mo_prepare_available_signatures` | Available signatures preserve recovery, customizer and import identities. | checker/lowering suites: pass |
| `src/check/signatures.c` / `mo_prepare_signatures` | Public partial-preparation refusal remains intact. | checker/lowering suites: pass |
| `src/check/recursion.c` / `mo_check_recursion` | Main’s SCC proof extends to generic promise bodies and callbacks. | checker/lowering suites: pass |
| `fuzz/fuzz_program.c` / `graph` | Fuzz assertions now cover constants/specs as well as ordinary functions. | fuzz_program: pass |
| `src/bytecode/assemble.c` / `mo_text_check_references` | Explicit numeric casts preserve primitive/host index bounds. | bytecode suites: pass |
| `src/bytecode/shorthand-function.c` / `library` | Explicit numeric casts preserve primitive/host signature lookup. | bytecode suites: pass |

## Shared and reused state

The constant graph keeps arena-stable symbol and initializer pointers; a paused
read re-enters through `mo_constant_by_symbol` before storing the completed
initializer. The import ledger retains both the selected origin and its parent,
crediting those same entries on re-lookup. Promise links are re-stored through
`spec_indices` after qsort; scoped ability parameter keys remain canonical when
proof actual types share storage.

Executed debug coverage includes the 5,000-constant chain, cross-module constants,
generic imported data/proofs, recursive nominal dependencies, capture aliases,
and independent source diagnostic recovery. `test_heap` also directly reuses a
record into the same `record` handle twice (`test_reuse_and_images`), then stores
the fields through that reused handle. `ci/check_cli.py` covers the fake Console
alias/snapshot/re-store chain; its full gate result is recorded separately.

## Five implementation conditions

Assertions, explicit limits, checked results, arena allocation, and strict
conversion warnings remain enabled. Sanitizers and every fuzz target run through
the unchanged CI gate. The trusted core and bytecode verification boundary are
unchanged; parser/Core/lowering/VM comparisons test the shared Lean protocol.
The full CI=1 gate exits 0 with no skips, including ASan/UBSan, every fuzz target, grammar, Lean comparisons, dependencies, library map, formatting and clang-tidy; see the final run record.

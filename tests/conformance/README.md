# Reference rule map

This is a map with a coverage guard, not conformance. It does not complete the
Phase 2 conformance item. Entries describe evidence and remaining gaps, not a
count of rules the implementation satisfies.

`rules.tsv` maps the Language Reference v0.1 to existing executable programs.
`reference.tsv` holds the corresponding reference text. Both files use the same
key, `SECTION/BLOCK`, in reference order. For example, `5.6/3` is the third
prose block under "5.6 Functions as values". Appendices use `A`, `B`, and `C`.

A block is a prose paragraph, list item, table data row, or fenced `text`
example. A paragraph can contain several requirements. Its programs and gap
explanation apply together; one program does not certify every sentence.
Mo code examples illustrate the surrounding rules rather than adding inventory
entries. The introductory version notice is not a language rule.

The index deliberately includes descriptive tables, the grammar summary, and
the roadmap. Their entries explain which claims a runnable program cannot
establish. A gap is not a passing conformance test.

## Read a section

```sh
python3 ci/check_conformance.py --section 5.6
python3 ci/check_conformance.py --section 14
```

The report joins both tables and shows the programs, remaining gaps, and matching
`tests/check/diagnostics/*.diag` fixtures where they exist. These diagnostic
fixtures pin the public diagnostic fields; the corresponding `tests/errors/`
programs pin the complete CLI error, including its locations and fixes.

## Columns

- `rule` is the reference block's key.
- `accepts` lists successful `tests/golden/` case names, separated by commas.
- `refuses` lists `tests/errors/eNNNN[-variant]` cases or golden cases that
  exit unsuccessfully, such as a failed example, promise, test, or `main` result.
- `stops` lists golden cases that stop with exit status 70. A runtime bug is
  different from a compiler refusal or an ordinary declared failure.
- `gap` explains any part the listed programs do not cover. It distinguishes
  unsupported language behavior, missing programs, claims tested by another
  driver, and claims that need work or allocation instrumentation.

An empty `refuses` or `stops` column does not mean every input is valid. It
means no such program is indexed. Where the reference requires refusal and the
suite lacks the relevant program, the gap names it.

## Checks

```sh
python3 ci/check_conformance.py
python3 ci/test_conformance.py
bash ci/check.sh gcc drivers
```

The map check validates coverage metadata, not language execution. It
fails on an unindexed block, changed reference text, duplicate or unknown keys,
missing source or expectation files, or a program in the wrong verdict column.
Each block needs an accepting program or a written gap. A refusal-only block
needs an explanation of why it has no accepting program.

The `drivers` group runs this check and its regression tests alongside
`ci/check_golden.py` and `ci/check_errors.py`. Those existing runners execute
every linked program and compare its exact output, error, and exit status.
The `gcc` group also runs the checker diagnostic-field fixtures.
No second expectation format or second language runner is introduced.

## Change the index

Map an existing golden or error program before adding a new one. If none
exercises the requirement, add a program to the existing corpus and write its
expectation by hand from the reference. If the compiler disagrees, record the
discrepancy in `docs/status/golden-gaps.md`; never adopt the wrong output.
Keep a gap for requirements that the new program still does not exercise.

After a reference edit, review the affected mappings, including blocks whose
ordinal changed. Then refresh only the reference-text inventory:

```sh
python3 ci/check_conformance.py --inventory > tests/conformance/reference.tsv
python3 ci/check_conformance.py
```

This command does not write programs, expectations, mappings, or exemptions.
An added block still fails until it has a `rules.tsv` entry.

Compiler and library changes can make a gap stale even when the reference has
not changed. Refresh affected gaps when a PR closes one, or during a periodic
review. Add programs with hand-written expectations for supported forms, and
remove only the claims those programs establish.

## Review against main on 24 October 2026

All 91 original gap entries were reviewed against main `4a40389`. The review
checked their linked programs, error fixtures and owning drivers, including
the new tracked-source formatter check. Seven entries now have complete
program mappings where they previously had gaps: `3.2/4`, `9.2/20`, `9.2/21`,
`11.1/3`, `11.1/8`, `14/3` and `14/16`. There are 84 remaining gap entries.

`ref-bigint` covers the restored native functions and conversion boundaries.
`ref-capability-fakes` covers Env snapshots and seeded Random streams.
`ref-abilities` now covers qualified Int comparison, comparisons across the
built-in Order families, Ordering/Entry/Map text and both Float exponent
thresholds. The existing `lib-unicode16` golden covers letter/digit
classification added on main. Formatter descriptions now name
`ci/check_format.py`; its preservation checks do not replace hand-written
layout expectations.

The remaining entries name absent standalone programs, partial matrices,
checks owned by other drivers, costs requiring instrumentation, nondeterministic
host failures, future-version claims, or confirmed compiler limitations.
Multiline case interpolation and direct calls on a nullary case such as
`Equal.to_text` now have the `txt-interpolation-case` and
`lang-dot-calls-cases` programs.

## Initial mapping

The initial mapping reuses the golden corpus, exact error programs, and their
diagnostic-field counterparts. The new `ref-shout` program is section 1's
complete example without source changes. `ref-evaluation` makes section 13.1's
evaluation order observable through Console. `ref-functions` exercises the
accepted closure and dot-call forms next to the existing refusal fixtures.
`ref-source`, `ref-operators`, `ref-time`, `ref-library-natives`, and
`ref-abilities` fill gaps in source layout, precedence, time boundaries, native
library operations, and the ability matrix.

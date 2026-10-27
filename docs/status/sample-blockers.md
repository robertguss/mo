# Sample blockers

## Current diagnostics

None: all 12 samples pass `mo check` with 0 errors.

## Regeneration and evidence

Build `mo`, then run:

```sh
build/clang/debug/mo check --json examples/<sample>.mo
```

Read `errors` from each totals record and assert `dropped == 0`.

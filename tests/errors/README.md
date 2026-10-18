# Error-code cases

One folder per case, `eNNNN` or `eNNNN-variant`, checked by `ci/check_errors.py`:

- `main.mo` (and any other modules) and `expected`, the exact standard error of
  `mo check tests/errors/<case>/main.mo`, which exits 1. Every diagnostic in it
  must carry the folder's code.
- A case with a `generated` file names a generator in `ci/check_errors.py` that
  writes a source too big to commit; the expected text then shows `main.mo`.
- A case may add `command` (a subcommand other than `check`), `args` and
  `stdout`.
- `unit.tsv` lists the codes no source program can raise, each with the unit
  test that pins it. A reason starting `UNTESTED:` marks a code nothing tests
  yet; the driver prints those so the gap stays visible.

Every code in `src/base/codes.h` must be in exactly one place. Nothing
regenerates `expected`: edit it by hand, and treat a wrong-looking message as a
bug to fix.

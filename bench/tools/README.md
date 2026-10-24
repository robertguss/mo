# The three tools

Phase 2 asks fresh agent sessions to write three real tools in Mo (decision D230): a link
checker, a Notion copy of the Language Reference, and a tally of how often agents fix `mo
check`'s errors on their next try. Each tool's task text and hidden tests are frozen here before
any session sees them (MO-61), so a later round can't drift towards what sessions happened to
write.

| Tool | Task text | Hidden tests |
| --- | --- | --- |
| `linkcheck` | [linkcheck/task.md](linkcheck/task.md) | 20, in 21 runs |
| `notioncopy` | [notioncopy/task.md](notioncopy/task.md) | 14, in 22 runs |
| `tally` | [tally/task.md](tally/task.md) | 16, in 19 runs |

Each tool's folder holds:

- `task.md`: the text a session reads. Its first paragraph replaces `TASK` in the learnability
  prompt (`learnability/final/prompt.md`), and the rest of the file follows it.
- `hidden/reference.py`: a plain Python program written to the task text's rules. It is how the
  expected outputs were checked, never what they were copied from: each small test's expected
  output was written by hand first.
- `hidden/tests/`: one folder per run, numbered as in the approved drafts in Notion (sub-pages of
  MO-61). A test that runs the tool more than once, such as the usage errors, has one folder per
  run under the same number. `grade.py` describes the files in a test folder.
- `SHA256SUMS`: the hash of `task.md` and of every file under `hidden/`.

Sessions must never see `hidden/`. Give a session only `task.md`, through the prompt, and run it
somewhere without a checkout of this repository.

The texts are the drafts Robert approved on 18 October 2026, with clarifying clauses added while
the tests were built, wherever an approved test needed a rule the draft didn't state: how a code
fence closes, indented reference definitions and links in headings (linkcheck); headings after
the title, how a long section splits, and `--limit` above 0 (notioncopy); and that every error is
reported on standard error (all three).

## Grading

```sh
python3 bench/tools/grade.py linkcheck answers/linkcheck.mo --mo build/gcc/debug/mo
python3 bench/tools/grade.py tally --reference     # run the reference instead
python3 bench/tools/grade.py --check               # what CI runs
```

Each test runs the program with `mo run PROGRAM ARGS...` in a fresh copy of the test's input
folder and compares standard output and the exit status exactly; the test folder says when
standard error or the files left behind are compared too. A program passes when every run
passes.

## Frozen means frozen

CI (`ci/check.sh`, the `static` group) runs `grade.py --check`: every `SHA256SUMS` must match its
files, and every reference must pass every one of its tests. Once a session has seen a task
text, neither the text nor its tests change. A change after that is a new decision, a new task
text, and a fresh round, and `grade.py --freeze TOOL` writes the new hashes.

## Notes on the tests

- **linkcheck.** The input folders are named `.input/`, so that once `linkcheck` runs over this
  repository (rule 1 skips folders starting with `.`), the tests' planted broken links aren't
  reported. Git can't store a folder named `.git`, so test 13's hidden folders are `.github/`
  and `.cache/`. Test 20 is the repository at the commit in `tests/20-mo-repository/commit`, with every
  Markdown file whole and every other file present but empty (only its existence matters), one
  link and one anchor broken on purpose, and a 10-second limit.
- **notioncopy.** Tests 1 and 2 convert `docs/reference/README.md` at the same commit. Converting
  the reference as it was on 12 October gives, byte for byte, the Notion copy made that day
  (`hidden/notion-copy-2026-10-12.txt`, 68,664 bytes); test 1's pieces, joined, differ from it
  only in the paragraph of reference 13.4 that MO-74 changed on 16 October. A piece always ends
  in a newline, so test 12's piece is `Wikipedia\n`; Adler-32's well-known `11e60398` is
  `Wikipedia` without it.
- **tally.** Every `output.jsonl` is what `mo check --json` or `mo test --json` printed at the
  commit in `hidden/mo-commit`, for the `src/` beside it; three tests edit one by hand: 09 adds
  a second diagnostic in the same function (`mo` reports `y + z` once), 14 an unknown record
  and field, and 15 truncates a line. Test 7's `fn first[T](...)` is how an
  agent used to other languages writes a generic function; Mo rejects it with E0024 at that line,
  and the error is in `first`, the same error as the next run's E0024 inside `first`'s body.

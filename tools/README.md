# The three Mo-written tools

linkcheck, notioncopy and tally were written in Mo by fresh agent sessions in the three tools'
final learnability round ([#181](https://github.com/robertguss/mo/pull/181)). The copies here are
maintained: they may change, and `ci/check_tools.py` (the `drivers` group of `ci/check.sh`) runs
each one on this repository. The round's frozen programs, grades and logs stay unchanged in
`learnability/tools-final/`, and the task texts and hidden tests in `bench/tools/`.

| Tool | Taken from | Changed since |
| --- | --- | --- |
| [linkcheck.mo](linkcheck.mo) | `learnability/tools-final/gpt-6.1-sol/linkcheck.mo` | heading text is trimmed before its anchor is made, and a test for that |
| [notioncopy.mo](notioncopy.mo) | `learnability/tools-final/gpt-6.1-sol/notioncopy.mo` | nothing |
| [tally.mo](tally.mo) | `learnability/tools-final/claude-code/tally.mo` | nothing |

## Why these versions

Each tool had two passing or near-passing versions; the one kept is the one that is right on the
inputs the round found in dispute.

- **linkcheck.** GPT-6.1 Sol's passed 23 of 23 in the round; Claude Code's failed tests 21 and 22
  (empty `#` fragments, combining marks and fullwidth punctuation in anchors). GPT-6.1 Sol's kept
  spaces around heading text, which hidden test 23 (#183) catches; trimming the text is a one-word
  change, and it now passes all 24 runs.
- **notioncopy.** Both passed 22 of 22. Claude Code's keeps a table separator line holding a tab,
  `| ---\t| --- |`, as a data row (the round's finding); GPT-6.1 Sol's drops it, as the reference
  does.
- **tally.** Both passed 19 of 19 and print the same tables on every recorded round. Claude Code's
  is kept because GPT-6.1 Sol's stops with `not allowed` on the final round's logs: one session
  ran `mo check` on a file outside its work folder, so the log's `src/` holds an absolute path,
  `src//tmp/s/bad.mo`, which the read-only folder capability refuses.

## How each is used

From the repository's root, with `mo` built:

```sh
mo run tools/linkcheck.mo                                  # every Markdown link in the repository
mo run tools/tally.mo LOGS                                 # a round's logs, from session.py collect
mo run tools/notioncopy.mo docs/reference/README.md OUT    # the reference, in pieces for Notion
python3 ci/check_tools.py build/gcc/debug/mo               # what the drivers group runs
```

- **linkcheck** checks every relative link and heading anchor in the repository's Markdown. The
  check runs it over a copy of the tracked files (Markdown whole, the rest empty), so untracked
  files don't change the answer, and fails on any broken link. Fix the link, or the heading it
  names.
- **tally** reports the first-retry numbers of a learnability round. The check collects each
  `learnability/*/sessions/` folder with `learnability/harness/session.py collect` and requires
  tally's table to match, byte for byte, the reference tally that `learnability/retry.py` uses
  for a round's report. A new round's logs are checked as soon as they are committed.
- **notioncopy** turns the Language Reference into the pieces that are pasted into its Notion
  copy, each under 12,000 bytes with its Adler-32 to confirm the paste. The check converts
  `docs/reference/README.md` and requires the same pieces and output as the reference
  converter.

Each tool's own tests (`mo test tools/NAME.mo`) and its frozen hidden tests
(`bench/tools/grade.py`) run in the same check. A change to a tool must keep both passing.

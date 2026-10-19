# The session-recording harness

Every learnability session of Phase 2 runs `mo` through this harness, which records each run as
`learnability/protocol.md` section 2 requires (MO-99). The agent types `mo` as usual and sees the
usual text; the harness keeps what `tally` and the round's `notes.md` need.

## Running a session

```sh
python3 learnability/harness/session.py start --mo FROZEN_MO --sha256 HASH SESSION
```

`HASH` is the binary's SHA-256 from the round's `freeze.md`; `start` refuses a binary with any
other hash, and a `SESSION` (or `--work` folder) that isn't empty. It prints the two lines the
session needs: put `SESSION/bin` first on the agent's `PATH`, and start the agent in
`SESSION/work`, the session's empty folder (`--work FOLDER` names another one). Then give the
agent its prompt. `start` doesn't start the agent itself: how a session is started depends on its
family and tool, which `freeze.md` records.

`SESSION` then holds:

| Path | What it is |
| --- | --- |
| `bin/mo` | the command the agent runs: a copy of `session.py` that records, then runs the frozen `mo` |
| `.mo/mo` | the frozen binary, copied and checked against `HASH` |
| `session.json` | the binary's hash, the work folder, and when the session started |
| `log/NNN/` | each `mo check`, `mo test` and `mo fix`, as the session log of `bench/tools/tally/task.md` |
| `runs/NNN/` | every `mo` command the agent ran, `mo run` and `mo guide` included |
| `work/` | the agent's files |

A run's `runs/` folder holds `argv` (a JSON list), `cwd` (relative to `work/` when inside it),
`started` (UTC), `seconds`, `status`, and what the agent saw: `stdout` and `stderr`, or `output`
when the agent's standard output and error were one file (a terminal, or `2>&1`), in which case
`mo` is given one pipe for both so their order is kept. Output reaches the agent as `mo` prints
it. When the run is in the log, `runs/NNN/log` names its number there. `notes.md` reads `runs/`;
`tally` reads `log/`. A wrapper that is killed mid-run leaves its `runs/` folder without `status`
and a folder in `.pending/`, and its run is not in the log.

## What goes in the log

- **`src/`** is a copy of every `.mo` file under `work/`, taken before the command runs.
- **`output.jsonl`** comes from a second run of the same command with `--json`, started beside
  the agent's own and run in the `src/` copy, with each argument that names a file inside `work/`
  rewritten relative to it. So every `file` in it is a path under `src/`, wherever the agent ran
  `mo` from and however it named the file, and the agent's own run is untouched. An argument
  naming a file outside `work/` is made absolute, so it is still that file; `src/` doesn't hold
  it, so `tally` finds its errors' functions in the file as it is when `tally` runs. Paths are compared after resolving
  symbolic links.
- **A refused command isn't logged.** A `check` or `test` whose JSON output doesn't start with a
  `version` record (a usage mistake, such as `mo check` with no file, exits 2 and prints nothing)
  checked no program, and logging it would count as a run that fixed every error. A `fix` that
  exits 2 is left out for the same reason. Both are still in `runs/`. Every other `fix` is
  logged, although on this build it changes nothing (E1400); protocol section 7 counts the errors
  that moves to `by_fix`.
- **A file that can't be read isn't logged either.** A misspelt name (`mo check mian.mo`) gives
  E0034 with a `file` of `null`: that run read no program, `tally`'s task text doesn't say what
  function such an error is in, and the next correct run would count it as fixed on the first
  try. So a `check` or `test` with an E0034 that has no file is kept in `runs/` only (Robert,
  19 October 2026). A missing imported module's E0034 names the importing file and is logged.
  Other diagnostics with no file come from the toolchain itself (a file over the size limit, an
  unreadable `lib/`, running out of memory, an internal step failing); they stay in the log, and
  `tally`'s reference stops on them, so a round with one looks at that run before tallying.
- **Numbers.** A run's `runs/` number is taken when it starts, and its log number when it
  finishes, so runs that overlap get distinct numbers in the order they finished.

## After the session

```sh
python3 learnability/harness/session.py check-log SESSION/log      # the log has tally's layout
python3 learnability/harness/session.py collect ROUND/logs SESSION...
```

`collect` copies each session's log to `ROUND/logs/<session folder's name>`, the folder `tally`
takes. `test_session.py` runs a session through the harness against a built `mo` and checks
that `bench/tools/tally/hidden/reference.py` reads its log to the table in `tally`'s example; CI
runs it in the `drivers` group.

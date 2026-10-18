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
`started` (UTC), `seconds`, `status`, and the `stdout` and `stderr` the agent saw. When the run
is in the log, `runs/NNN/log` names its number there. `notes.md` reads `runs/`; `tally` reads
`log/`.

## What goes in the log

- **`src/`** is a copy of every `.mo` file under `work/`, taken before the command runs.
- **`output.jsonl`** comes from a second run of the same command with `--json`, from `work/`, with
  each argument that names a file inside `work/` rewritten relative to it. So every `file` in it
  is a path under `src/`, wherever the agent ran `mo` from and however it named the file, and the
  agent's own run is untouched. A `mo test` therefore runs twice. A file outside `work/` keeps the
  path the agent gave, and `tally` finds no function for its errors.
- **A refused command isn't logged.** A `check` or `test` whose JSON output doesn't start with a
  `version` record (a usage mistake, such as `mo check` with no file, exits 2 and prints nothing)
  checked no program, and logging it would count as a run that fixed every error. A `fix` that
  exits 2 is left out for the same reason. Both are still in `runs/`.
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

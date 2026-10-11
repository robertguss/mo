# The CI orb

CI for robertguss/mo runs in a dedicated Amp orb, on request (decisions D208
and D214). Nothing runs on GitHub: there is no GitHub Actions workflow, no
webhook and no commit status. The checks themselves are `ci/check.sh`, the same
script every agent runs before pushing.

## Asking for a CI run

From an implementation thread: push the branch, open the pull request, then send
the CI orb's thread ("Mo CI orb",
https://ampcode.com/threads/T-01a12c75-2065-769a-bed4-8687b915c8db) a message
with `send_thread_message`:

> Run CI on robertguss/mo pull request #N, and reply to <your thread ID>.

The CI orb runs `bash ci/orb/run.sh N` and replies with the result: pass or
fail, the commit it checked and, on a failure, the end of the log. The script
also comments the result on the pull request, so GitHub keeps a record. A result
covers only the commit it names, so ask again after pushing more commits.

A pull request merges when its latest commit has a passing CI orb run and the
reviewer agent has approved it (Working Agreement).

## What `run.sh` does

- Looks up the pull request's head commit, and refuses pull requests that are
  closed or come from a fork.
- Checks that commit out in a fresh worktree under `~/mo-ci/work`.
- Runs the commit's own `ci/check.sh` with `CI=1`, under `env -i` with only
  `PATH`, a fresh `HOME`, `LANG` and `CI`, so the check doesn't inherit the
  orb's credentials. That is not a hard boundary: the check runs as the same
  user and could read another process's environment under `/proc`, which is
  acceptable only because just Robert and his agents can push here. The check
  runs in its own process group with a time limit (`MO_CI_TIMEOUT_SECONDS`,
  1800 by default). Stopping the script with TERM or INT stops everything the
  check started; a script killed with KILL can't, and its check runs on until
  its time limit. The next run clears what it left behind.
- Keeps the full log in `~/mo-ci/logs/robertguss__mo-pr<N>-<sha>.log`.
- Comments the result on the pull request, prints a summary, and exits with the
  check's status: 0 when every check passed. It exits with 64 for a usage
  error and 69 when it could not run the check at all.

Runs share one clone, so a second run waits for the first.

## The CI orb itself

- It needs the toolchain from `.agents/setup`; re-run that when it changes,
  because under `CI=1` a missing tool fails the check.
- Its own checkout stays on `main`; `git pull` before a run, so `run.sh` is
  current. Each run checks the pull request's code, not `main`'s.
- Never archive its thread: it is the CI.

## Matching the CI orb locally

The formatting check (`make format-check`, part of `ci/check.sh`) uses the CI
orb's clang-format, version 14. Other versions can format the same code
differently, so a check that passes with your clang-format can fail in the CI
orb. Check `clang-format --version`, and if it is not 14, put version 14 first
on your `PATH`:

```sh
# Debian or Ubuntu
sudo apt install clang-format-14
PATH=/usr/lib/llvm-14/bin:$PATH make format

# Any platform with Python (installs into ~/.local/bin)
pipx install clang-format==14.0.6
```

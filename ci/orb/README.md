# The CI orb

Mo has no hosted CI: no Mo check runs on GitHub, and `main` requires no status
check. `ci/check.sh` runs every check locally. The CI orb is an optional runner
that runs it on a pull request when asked (decisions D208 and D214); an
equivalent verified Linux environment, including a Docker container on the Mac,
works too.

## Asking for an optional CI-orb run

From an implementation thread: push the branch, open the pull request, then send
the CI orb's thread ("Mo CI orb",
https://ampcode.com/threads/T-01a13551-abe5-760e-bcf3-90e87c4466a1) a message
with `send_thread_message`:

> Run CI on robertguss/mo pull request #N, and reply to <your thread ID>.

The CI orb runs `bash ci/orb/run.sh N` and replies with the result: pass or
fail, the commit it checked and, on a failure, the end of the log. The script
also comments the result on the pull request, so GitHub keeps a record. A result
identifies the commit it tested; apply the evidence rules below to later changes.

## Merge verification

AGENTS.md "Checks and merging" holds the merge rule. Record in the PR the
commit you checked and each group you ran, with its result; a check you did not
run is named as not run, never claimed. A full `CI=1 bash ci/check.sh` run, here
or in the CI orb, is the way to check a change to the slow groups' own inputs
(the CBMC harnesses, `formal/`, a fuzz target) before merging it.

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

## Long fuzzing

`ci/check.sh`'s `fuzz` group fuzzes each target only briefly. For a longer run,
the CI orb can run the full check by hand with `bash ci/orb/nightly.sh`:
`ci/check.sh` on `main`'s latest commit, each target fuzzed for five minutes
(`MO_CI_FUZZ_SECONDS`, 300 by default), keeping the log and any crashing inputs
under `~/mo-ci/logs`. It has its own clone and lock under `~/mo-ci/nightly`, so
pull requests' runs don't wait the hour or more it takes. Add any crashing input
to the target's corpus when you fix it.

## The CI orb itself

- It needs the toolchain from `.agents/setup`; re-run that when it changes,
  because under `CI=1` a missing tool fails the check.
- Its own checkout stays on `main`; `git pull` before a run, so `run.sh` is
  current. Each run checks the pull request's code, not `main`'s.
- Never archive its thread: the orb lives in it.

## Matching clang-format 14 locally

The formatting check (`make format-check`, part of the `static` group) expects
clang-format version 14, which the CI orb has. Other versions can format the
same code differently, so a check that passes with your clang-format can fail
for anyone using 14. Check `clang-format --version`, and if it is not 14, put version 14 first
on your `PATH`:

```sh
# Debian or Ubuntu
sudo apt install clang-format-14
PATH=/usr/lib/llvm-14/bin:$PATH make format

# Any platform with Python (installs into ~/.local/bin)
pipx install clang-format==14.0.6
```

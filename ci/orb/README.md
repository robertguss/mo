# The CI orb

CI for robertguss/mo runs `ci/check.sh`. Robert removed the requirement to use
the dedicated Amp CI orb: an equivalent verified Linux environment, including
a Docker container on the Mac, is also supported. The orb remains an optional
runner (decisions D208 and D214). GitHub Actions also runs `ci/check.sh` on
every pull request, its groups as parallel jobs (`.github/workflows/check.yml`).
The full verification requirements are unchanged.

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

A pull request needs a passing full `CI=1 bash ci/check.sh` run with no missing
checks, in the CI orb or an equivalent verified Linux environment, plus reviewer
agent approval and Robert’s merge authorization. Record the tested commit,
commands, toolchain, exit status and retained log in the PR. Sanitizers, short
fuzzing, Lean comparisons and every other gate in `ci/check.sh` remain required.

Changed source, test, oracle or build inputs require rechecking; documentation
consumed by checks counts as an input too. A documentation-only follow-up may retain a prior passing result when its diff is reviewed,
`git diff --check` passes and recorded hashes prove the executable/test/build
inputs unchanged. Name both revisions; never claim a fresh run on an untested
commit. A specific CI-orb result is not a merge prerequisite. Required GitHub
checks and branch protection must still pass; this policy does not authorize
bypassing them.

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

## Nightly fuzzing

A pull request's check fuzzes each target for five seconds (`FUZZ_SECONDS` in
the `Makefile`), which keeps a run to a few minutes. Once a night the CI orb runs
`bash ci/orb/nightly.sh`: the same `ci/check.sh` on `main`'s latest commit, with
each target fuzzed for five minutes (`MO_CI_FUZZ_SECONDS`, 300 by default). It
keeps the log, and any crashing inputs, under `~/mo-ci/logs`, and sends the
coordinating thread one line with the result, or the end of the log on a
failure. A failure there is a bug on `main`; fix it in a pull request, and add
the crashing input to the target's corpus. The nightly run has its own clone
and lock under `~/mo-ci/nightly`, so pull requests' runs don't wait the hour or
more it takes.

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

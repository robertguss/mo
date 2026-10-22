# The CI orb

CI for robertguss/mo runs `ci/check.sh`. Robert removed the requirement to use
the dedicated Amp CI orb: an equivalent verified Linux environment, including
a Docker container on the Mac, is also supported. The orb remains an optional
runner (decisions D208 and D214).

## The two tiers

While Mo is experimental and has no outside users, CI runs in two tiers, so a
pull request gets a merge decision in minutes and a broken `main` is found the
next morning rather than prevented at every merge.

- **Fast, on every pull request and push to `main`** (`.github/workflows/check.yml`):
  `gcc`, `clang`, `static` and `drivers`, which finish in a few minutes. They decide the required `check` status. A change that touches only
  documents no check reads runs none of them, and `check` still reports.
- **Slow, once a night against `main`** (`.github/workflows/nightly.yml`): `cbmc`,
  `lean`, `fuzz` with five minutes per target, and `sanitize` together with the
  driver checks over the sanitize builds. A scheduled night with no new commit
  on `main` since the last finished run is skipped. `.github/workflows/differential.yml`
  also runs every night, comparing generated Core programs on the C VM and in Lean.

`ci/ci_groups.sh` holds both lists; `ci/test_ci_groups.sh` checks that together
they are every group `ci/check.sh` has. Locally, the qgate pre-push hook runs the
`gcc` group (`qgate.toml`).

When a nightly group fails, `ci/nightly.sh` files an issue labelled `nightly`,
titled "Nightly: <group> failing on main", with the tested commit, the commits
since the previous nightly run and a link to the run. A night it stays red
adds a comment; the night it passes again closes the issue. An open `nightly`
issue is a bug on `main`: fix it in a pull request like any other task.

## Restoring the strict gate

Once Mo has outside users or real applications, go back to running every group
before a merge: make `ci/ci_groups.sh` print the nightly groups with the fast
ones (and update `ci/test_ci_groups.sh`), and make the `check` tool in
`qgate.toml` run `CI=1 bash ci/check.sh gcc clang sanitize static drivers` again.
The nightly workflow can stay as the place long fuzzing runs.

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

A pull request merges on a green hosted `check` status (the fast tier above),
plus reviewer agent approval and Robert's merge authorization. Record in the PR
the commit `check` ran on and any groups you ran locally for the change, with
their results; a check you did not run is named as not run, never claimed.
Required GitHub checks and branch protection must pass; this policy does not
authorize bypassing them. A full `CI=1 bash ci/check.sh` run, here or in the CI
orb, is still the way to check a change to the slow groups' own inputs (the
CBMC harnesses, `formal/`, a fuzz target) before merging it.

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

A pull request's check doesn't fuzz. The nightly workflow fuzzes each target for
five minutes (`FUZZ_SECONDS=300`), keeps any crashing inputs as an artifact of
the run, and files a `nightly` issue on a failure; fix it in a pull request, and
add the crashing input to the target's corpus. The CI orb can run the same full
check by hand with `bash ci/orb/nightly.sh`: `ci/check.sh` on `main`'s latest
commit, each target fuzzed for five minutes (`MO_CI_FUZZ_SECONDS`, 300 by
default), keeping the log and any crashing inputs under `~/mo-ci/logs`. It has
its own clone and lock under `~/mo-ci/nightly`, so pull requests' runs don't
wait the hour or more it takes.

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

# The CI orb

CI for robertguss/mo runs in a dedicated Amp orb, not in GitHub Actions
(decision D208). The checks themselves are `ci/check.sh`, the same script every
agent runs before pushing.

```
GitHub push
  └─▶ webhook "github" in the mo-ci plugin (.amp/plugins/mo-ci.ts)
        checks the signature, queues one job per commit in ~/mo-ci/queue,
        and keeps the orb awake while a job is waiting or running
  └─▶ orb service "mo-ci-runner" (ci/orb/runner.sh)
        checks out the commit, runs its ci/check.sh with CI=1,
        posts the "mo-ci" commit status, and on failure comments the
        end of the log on the commit's open pull requests
```

The plugin loads in every orb working on this repository but does nothing
unless `~/mo-ci/enabled` exists, which only the CI orb has. The CI orb's own
checkout stays on `main`: the plugin and the runner come from it, while each
job runs the checks of the commit under test.

> **Do not archive the CI orb's thread.** Archiving it makes the webhook URL
> return 404 and pauses delivery, so nothing is built until the thread is
> restored, and a push made meanwhile is not built later by itself: redeliver it
> from the webhook's Recent Deliveries page on GitHub, or push again.

## Setting up the CI orb

Done once, by the CI orb's agent, in the orb's checkout of robertguss/mo on
`main`. Never print the webhook URL or the secret.

1. Run `.agents/setup` if the toolchain is missing.
2. Create the secret and the switch:
   ```sh
   mkdir -p ~/mo-ci && chmod 700 ~/mo-ci
   openssl rand -hex 32 > ~/mo-ci/webhook-secret && chmod 600 ~/mo-ci/webhook-secret
   touch ~/mo-ci/enabled
   ```
3. Reload plugins (the `reload_plugins` or `load_plugin` tool, or
   `amp orb restart-processes`). The plugin registers the webhook and writes
   its URL to `~/mo-ci/webhook-url`.
4. Check that the GitHub token is in the environment (see "The GitHub token"
   below): `[ -n "${MO_CI_GITHUB_TOKEN:-}" ] && echo set`. Then start the runner:
   ```sh
   amp orb service start mo-ci-runner --command "bash $PWD/ci/orb/runner.sh"
   ```
5. Point GitHub at the webhook, reading both values from their files. Subscribe
   to `push` events only: every pull request in this repository comes from one
   of its own branches, so its head commit always arrives as a push, and a
   `pull_request` event would only repeat a commit that is already queued.
   Forks, the only pull requests whose commits would not arrive as pushes, are
   not accepted.
   ```sh
   jq -n --rawfile url ~/mo-ci/webhook-url --rawfile secret ~/mo-ci/webhook-secret \
     '{name: "web", active: true, events: ["push"],
       config: {url: ($url | rtrimstr("\n")), content_type: "json",
                secret: ($secret | rtrimstr("\n")), insecure_ssl: "0"}}' |
     gh api repos/robertguss/mo/hooks --input - --jq '.id'
   ```
6. Check it end to end: push a branch with a small change, then watch
   `amp orb service logs mo-ci-runner` and the commit's `mo-ci` status.

## The GitHub token

Amp's own GitHub credential in orbs can push, open pull requests and comment,
but GitHub refuses it for commit statuses and check runs ("Resource not
accessible by integration"). So the runner posts statuses, and comments on
failures, with a dedicated token:

- **What it is:** a fine-grained personal access token with repository access
  to robertguss/mo only, and two permissions: Commit statuses (read and write)
  and Pull requests (read and write). Nothing else.
- **Where it lives:** the Amp project secret `MO_CI_GITHUB_TOKEN` on the
  robertguss/mo project. After adding or rotating it, run
  `amp orb restart-processes` in the CI orb so the environment picks it up, then
  restart the runner.
- **Who sees it:** the runner passes it to `gh` for its own API calls only.
  Checks run under `env -i` and never see it. Project secrets reach every orb on
  this project, so other agents must never use it: a commit status means "the
  CI orb ran the checks", and nothing else may post one.
- **When it runs out:** fine-grained tokens expire. When statuses stop
  appearing and the runner's log says the token lacks access, rotate it.

Without the token, the runner still runs every check and logs the result, but
GitHub shows no `mo-ci` status.

## How jobs run

**One build per commit.** A job is a file named `<owner>__<repo>__<sha>.json`
that moves from `~/mo-ci/queue` to `running` to `done`. The plugin writes it to
a temporary name and renames it into `queue`, so the runner never sees a
half-written job, and it skips a commit whose file is already in `queue`,
`running` or `done`. A redelivered webhook, or a new branch at a commit that was
already built, adds nothing. To build a commit again, delete its file from
`~/mo-ci/done` and redeliver its push from GitHub (or push a new commit).

**Restarts.** When the runner starts, it moves every job left in `running` back
to `queue`, so a job that a restart interrupted runs again from the beginning
(its commit shows `pending` again) instead of staying pending forever. The
check runs in its own process group. When the runner is stopped with TERM or
INT, it kills that whole group, so no `make` or fuzzer outlives it. A runner
killed with KILL cannot clean up: its orphaned check keeps running until it
finishes or reaches the timeout.

**The check's environment.** The orb's environment holds API keys, so the check
gets none of it. It runs under `env -i` with only `PATH`, `HOME` (a fresh
directory under `~/mo-ci/work`, removed afterwards), `LANG=C.UTF-8` and `CI=1`.

**Failures.** A check that runs longer than `MO_CI_TIMEOUT_SECONDS` (1800 by
default) shows as "Timed out after Ns". Exit status 137 shows as "Killed after
Ns (timeout or out of memory)", because the kernel's out-of-memory killer looks
the same as a timeout's last resort.

**Keeping the orb awake.** While `queue` or `running` holds any `.json` file,
the plugin holds a `keepAlive()` lease, so the orb is not paused under a running
check. It takes the lease right after queuing a job, looks at the two
directories every 10 seconds (and once when it loads, for jobs already waiting
when the orb wakes up), and releases the lease when both are empty. Unloading
the plugin releases it too. If `keepAlive()` fails, the plugin logs that and
carries on: the webhook still queues jobs. Jobs waiting for a stopped runner
keep the orb awake, so restart the runner or delete the job files.

## After changing the plugin or the runner

In the CI orb: `git pull` on `main`, reload plugins, and
`amp orb service restart mo-ci-runner`. If `.agents/setup` changed, run it too:
a job whose `ci/check.sh` needs a tool the orb lacks fails, because under
`CI=1` a missing tool is an error.

## Debugging a failed run

Each run's full log is in the CI orb at `~/mo-ci/logs/<owner>__<repo>-<sha>.log`,
for example `~/mo-ci/logs/robertguss__mo-<sha>.log`, and failures comment the
last 60 lines on the pull request. To reproduce a failure in the same
environment, ask the CI orb's thread to run `bash ci/check.sh` on that commit.

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

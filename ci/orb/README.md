# The CI orb

CI for robertguss/mo runs in a dedicated Amp orb, not in GitHub Actions
(decision D208). The checks themselves are `ci/check.sh`, the same script every
agent runs before pushing.

```
GitHub push or pull request
  └─▶ webhook "github" in the mo-ci plugin (.amp/plugins/mo-ci.ts)
        checks the signature, queues one job per commit in ~/mo-ci/queue
  └─▶ orb service "mo-ci-runner" (ci/orb/runner.sh)
        checks out the commit, runs its ci/check.sh with CI=1,
        posts the "mo-ci" commit status, and on failure comments the
        end of the log on the commit's open pull requests
```

The plugin loads in every orb working on this repository but does nothing
unless `~/mo-ci/enabled` exists, which only the CI orb has. The CI orb's own
checkout stays on `main`: the plugin and the runner come from it, while each
job runs the checks of the commit under test.

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
4. Start the runner:
   ```sh
   amp orb service start mo-ci-runner --command "bash $PWD/ci/orb/runner.sh"
   ```
5. Point GitHub at the webhook, reading both values from their files:
   ```sh
   jq -n --rawfile url ~/mo-ci/webhook-url --rawfile secret ~/mo-ci/webhook-secret \
     '{name: "web", active: true, events: ["push", "pull_request"],
       config: {url: ($url | rtrimstr("\n")), content_type: "json",
                secret: ($secret | rtrimstr("\n")), insecure_ssl: "0"}}' |
     gh api repos/robertguss/mo/hooks --input - --jq '.id'
   ```
6. Check it end to end: push a branch with a small change, then watch
   `amp orb service logs mo-ci-runner` and the commit's `mo-ci` status.

## After changing the plugin or the runner

In the CI orb: `git pull` on `main`, reload plugins, and
`amp orb service restart mo-ci-runner`.

## Debugging a failed run

Each run's full log is in the CI orb at `~/mo-ci/logs/<repo>-<sha>.log`, and
failures comment the last 60 lines on the pull request. To reproduce a failure
in the same environment, ask the CI orb's thread to run `bash ci/check.sh` on
that commit.

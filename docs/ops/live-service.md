# The live service behind the review door

The task queue (`services/task-queue/main.mo`) runs on its own exe.dev VM behind
the review door. Agents change it only through its endpoint, and Robert answers
every change that waits for him. This runbook covers the host, the secrets, the
agents' side, Robert's reviews, and the evidence for each access check. The
install commands are the ones in [the task queue runbook](task-queue.md); this
document gives only what differs.

## Robert's choices

Recorded on 2026-11-15.

- The service is the task queue. Agents use it for real work: they track this
  repository's follow-up work in it (residual risks, learnings to capture,
  Phase 7 items), so its changes come from use.
- The minimum of real use is 14 days of continuous running and 10 real changes
  through the door, at least 3 of them held and answered by Robert. The planted
  change does not count.
- Agents reach the service through the VM's public HTTPS route and a loopback
  relay, not an SSH account limited to forwarding. exe.dev's SSH proxy logs into
  the VM with its own key, so no restriction on an agent's key reaches the VM's
  sshd, and any key an agent held would open a shell.

Changed on 2026-11-17.

- The minimum of real use no longer gates the phase. Fourteen days without
  other development costs more than the record is worth now. Phase 6 closes on
  automated evidence instead: the door's proof run, the kill loop, the
  simulation and review checks, the models, the CBMC proofs and long fuzz runs,
  recorded in [the run record](../status/phase6-run.md).
- That evidence shows the door's mechanism works. It does not show how Robert
  judges real changes, so his verdict on the setting is "not yet judged", the
  setting stays `everything`, and real use through the door is open work.
- The host stays up. Its first approvals and the access checks marked "not yet
  run" below wait until that work resumes; until then the development VM's key
  is still on the account and no change is pushed at the host.

## Who can do what

| Who | Reaches | Holds |
| --- | --- | --- |
| Agents, on the development VM | `https://$HOST/mcp` through the relay | the service token and the upgrade credential |
| Robert, on his own login on the host | the host, its data folder and its journal | all three secrets |

Restarting the service on a new program, starting it in a fresh data folder,
`mo history`, replay and query on the data folder, and the journal are Robert's.
The agent guide's "Work on a service behind the door" states the same rule.

## The deployment

The host is `mo-queue`, serving at `https://mo-queue.exe.xyz/mcp` since
2026-11-16 from `main` at `816a647322759a1bdb164f42000fe5e053a2f624`. Its `mo`
has sha256 `ce1018cdeab28fecd9d968c9a874ba57530a4b6ba3cbea35cf81ae4a6da2c48f`.

An agent built it with the development VM's exe.dev key, which opens a shell on
every VM of Robert's account. The development VM is not on that account, so the
key cannot be limited to it with `ssh-key add --vm=`. Once the host runs, Robert
removes the key from his own machine with `ssh exe.dev ssh-key remove NAME`,
then replaces the approval credential on the host before any approval exists:

```sh
sudo sh -c 'umask 077; python3 -c "import secrets; print(\"QUEUE_APPROVAL=\" + secrets.token_urlsafe(32))" > /etc/task-queue/approval'
sudo systemctl restart task-queue
```

The credential written at install time was read once onto the development VM,
into one `mo review` process, to check the review listing through the relay; no
approval was ever made with it.

The VM image also runs exe.dev's Shelley coding agent at
`https://mo-queue.shelley.exe.xyz`, behind Robert's exe.dev login. It has a
shell on the host, so no agent is ever pointed at it.

## Install

Robert creates the VM (`ssh exe.dev new --name=NAME`), sets `SHA` to a `main`
commit that holds the door and `HOST` to `NAME.exe.xyz`, and runs the task queue
runbook's Install steps 1 to 5 with these changes.

Step 4 also writes the upgrade credential into the environment file that clients
copy, and the approval credential into a root-only file of its own that no client
copies:

```sh
sudo sh -c 'umask 077; python3 -c "import secrets; print(\"QUEUE_UPGRADE=\" + secrets.token_urlsafe(32))" >> /etc/task-queue/env'
sudo sh -c 'umask 077; python3 -c "import secrets; print(\"QUEUE_APPROVAL=\" + secrets.token_urlsafe(32))" > /etc/task-queue/approval'
```

Step 5's unit reads both files and starts the queue with both credentials:

```ini
EnvironmentFile=/etc/task-queue/env
EnvironmentFile=/etc/task-queue/approval
ExecStart=/opt/mo/$SHA/build/gcc/debug/mo serve /opt/mo/$SHA/services/task-queue/main.mo --port 8000 --data /var/lib/task-queue --allow-host $HOST --upgrade-token-env QUEUE_UPGRADE --approval-token-env QUEUE_APPROVAL
```

The queue starts on `everything`, so every push waits for Robert.

## First approvals

Robert lists the running program's items and approves the ones he accepts, from
his login on the host:

```sh
review() {
  sudo sh -c '. /etc/task-queue/env; . /etc/task-queue/approval; export QUEUE_TOKEN QUEUE_APPROVAL
    exec "$0" review --url http://127.0.0.1:8000/mcp --token-env QUEUE_TOKEN \
    --approval-token-env QUEUE_APPROVAL "$@"' /opt/mo/$SHA/build/gcc/debug/mo "$@"
}
review --items
review --base PROGRAM --approve DIGEST...
```

## The agents' side

Robert copies the environment file to the development VM with the task queue
runbook's Clients recipe for a client that cannot SSH to the VM. It carries
`QUEUE_TOKEN` and `QUEUE_UPGRADE` and never `QUEUE_APPROVAL`. MCP clients
connect to `https://$HOST/mcp` as that recipe shows. `mo push`, `mo fetch` and
`mo review` speak plain HTTP to loopback only, so an agent starts the relay and
points them at it:

```sh
python3 docs/ops/live-relay.py 8000 https://$HOST &
mo push NEW/main.mo --url http://127.0.0.1:8000/mcp --base DIGEST \
  --token-env QUEUE_TOKEN --upgrade-token-env QUEUE_UPGRADE
mo fetch http://127.0.0.1:8000/mcp --export state --out state.moex --token-env QUEUE_TOKEN
```

An export records the exact `mo` executable that wrote it, and the build bakes
in its own path, so replay and query need the deployed build at the deployed
path. The development VM builds it with the Install's step 2 and the deployed
`SHA`; gcc 13.3.0 on both VMs gives the same sha256.

A held push exits 3 and prints `held ID`. The agent follows it with
`mo-upgrade-status`, never by pushing again, and tells Robert the id.

## Robert's reviews

On the host, with the `review` function above:

```sh
review --out /tmp/review-ID
review --yes ID
review --no ID
```

`--out` prints the intent difference, writes the held source to `pending` and the
running source to `active`, and prints `review ID` last. Answer only that id.
When Robert rejects a change that passed every check, the agent's next proposal
carries a promise or example that the rejected program fails.

## When it stops

The agents cannot restart it. Robert reads the journal as the task queue
runbook's When it stops says, restarts it with `sudo systemctl start task-queue`,
and runs any checked restart onto a new program himself, with `--upgrade` and,
when it changes approved intent, `--review ID`.

## Access checks

Each check is run once on the deployed host and its result recorded here.

| Check | Result |
| --- | --- |
| The development VM's key cannot open a shell on the host: `ssh $HOST true` from it is refused. | not yet run |
| A port other than the service's is not reachable through the public route. | 2026-11-16: a server on the host's port 3000 serving a marker page; `https://mo-queue.exe.xyz:3000/` from the development VM answered 307 to exe.dev's login, without the marker. Port 8000 answered 401. |
| The development VM's shells and client configuration hold no `QUEUE_APPROVAL`. | 2026-11-16: `~/.config/task-queue/env.sh` holds `QUEUE_TOKEN` and `QUEUE_UPGRADE` only; no shell variable, `~/.config`, `~/.zshenv` or `~/.claude.json` names it. |
| `mo fetch` through the relay returns the same state as a fetch on the host. | 2026-11-16: after one job was submitted, `mo query` with a `payloads` function returned the same list over both exports. The query needed the deployed build (above). |
| The review listing through the relay matches the one on the host. | 2026-11-16: `mo review --items` gave the same 342 lines both ways, so exe.dev's proxy passes `Mo-Approval-Authorization`. The host cannot reach its own public route, so this ran from the development VM. |
| A failure fetched through the relay replays off the host with the same `mo` build and the deployed source. | not yet run |
| A push from the agents' side is held on the host, and `mo-upgrade-status` shows it pending. | not yet run |
| One held push is answered by Robert on the host, and the decision shows in `mo history`. | not yet run |

`ci/check_upgrade_remote.py` checks the relay against a local service on every
run of the `drivers` group.

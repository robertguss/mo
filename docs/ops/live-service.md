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

## Who can do what

| Who | Reaches | Holds |
| --- | --- | --- |
| Agents, on the development VM | `https://$HOST/mcp` through the relay | the service token and the upgrade credential |
| Robert, on his own login on the host | the host, its data folder and its journal | all three secrets |

Restarting the service on a new program, starting it in a fresh data folder,
`mo history`, replay and query on the data folder, and the journal are Robert's.
The agent guide's "Work on a service behind the door" states the same rule.

## Before the host exists

The development VM's exe.dev key must not open a shell on the host. Robert
scopes it to that VM from his own machine, by removing it with
`ssh exe.dev ssh-key remove` and adding the same public key back with
`ssh exe.dev ssh-key add --vm=DEVELOPMENT_VM`.

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
| A port other than the service's is not reachable through the public route. | not yet run |
| The development VM's shells and client configuration hold no `QUEUE_APPROVAL`. | not yet run |
| `mo fetch` through the relay returns the same state as a fetch on the host. | not yet run |
| The review listing through the relay matches the one on the host. | not yet run |
| A failure fetched through the relay replays off the host with the same `mo` build and the deployed source. | not yet run |
| A push from the agents' side is held on the host, and `mo-upgrade-status` shows it pending. | not yet run |
| One held push is answered by Robert on the host, and the decision shows in `mo history`. | not yet run |

`ci/check_upgrade_remote.py` checks the relay against a local service on every
run of the `drivers` group.

# The task queue on exe.dev

**Retired on 2026-11-02.** Phase 3 now closes on the recovery check in
`mo test` and the kill loop, both run on the development VM, and nothing
depended on the deployed queue on `turnpike-ferret`. Its 48-hour idle check,
started at 2026-11-02 13:18 UTC, was abandoned, not completed.
`docs/status/phase3-run.md` records the teardown. The rest of this runbook
stays as the record of how the queue was deployed.

Mo's task queue (`services/task-queue/main.mo`) ran under systemd on a
dedicated exe.dev VM, from a pinned build, behind the VM's public HTTPS route.
Agent threads reached it at `https://VM.exe.xyz/mcp` with a bearer token. The
deployment and its drill results are in `docs/status/phase3-run.md`.

Run the commands below on the queue VM as a user with sudo, unless a step says
otherwise. Set these two first, in every shell you use:

```sh
SHA=<the main commit to deploy, all 40 characters>
HOST=<the VM's name>.exe.xyz
```

## Never edit the tree

`mo` reads `lib/` from the path it was built in, and saved data is tied to the
exact compiled program. Any edit under `/opt/mo/$SHA` can refuse the queue's
data at its next start. The tree is root-owned and read-only, so leave it alone.
To upgrade, see Upgrade below.

## Install

Step 1. The route. From a machine signed in to exe.dev, point the proxy at port 8000
and make the route public. Mo's token is the only gate.

```sh
ssh exe.dev share port VM 8000
ssh exe.dev share set-public VM
```

Step 2. The pinned tree, built in place. The build bakes in `/opt/mo/$SHA`, so it
must never move.

```sh
sudo git clone --quiet https://github.com/robertguss/mo /opt/mo/$SHA
sudo git -C /opt/mo/$SHA checkout --quiet --detach $SHA
sudo make -C /opt/mo/$SHA -j"$(nproc)" CC=gcc build/gcc/debug/mo
sudo chown -R root:root /opt/mo/$SHA
sudo chmod -R a-w /opt/mo/$SHA
```

Step 3. The user and the data folder. Create the folder once, by hand: `mo serve`
never creates one, so a missing folder stops the queue instead of starting
an empty one.

```sh
sudo useradd --system --no-create-home --shell /usr/sbin/nologin taskqueue
sudo install -d -m 0700 -o taskqueue -g taskqueue /var/lib/task-queue
```

Step 4. The token. It is generated straight into a root-only file and is never
printed, echoed, typed or pasted. An agent's transcript is a log, so a token
shown to an agent is a leaked token.

```sh
sudo install -d -m 0700 -o root -g root /etc/task-queue
sudo sh -c 'umask 077; python3 -c "import secrets; print(\"QUEUE_TOKEN=\" + secrets.token_urlsafe(32))" > /etc/task-queue/env'
```

Step 5. The unit. systemd restarts the queue after a crash and leaves it down after
a supervised stop, a startup refusal or `systemctl stop`.

```sh
sudo tee /etc/systemd/system/task-queue.service >/dev/null <<EOF
[Unit]
Description=Mo task queue
After=network.target
StartLimitIntervalSec=600
StartLimitBurst=5

[Service]
User=taskqueue
Group=taskqueue
EnvironmentFile=/etc/task-queue/env
ExecStart=/opt/mo/$SHA/build/gcc/debug/mo serve /opt/mo/$SHA/services/task-queue/main.mo --port 8000 --data /var/lib/task-queue --allow-host $HOST
Restart=on-failure
RestartPreventExitStatus=1 2 69
RestartSec=5s
TimeoutStopSec=40s
ProtectSystem=strict
ReadWritePaths=/var/lib/task-queue
PrivateTmp=yes
ProtectHome=yes
NoNewPrivileges=yes

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now task-queue
```

## Run and check

```sh
sudo systemctl status task-queue
sudo systemctl stop task-queue
sudo systemctl start task-queue
systemctl show task-queue -p ActiveState -p MainPID -p NRestarts -p ExecMainStatus
journalctl -o cat -u task-queue
journalctl -o cat -u task-queue | grep '^mo serve: failure:'
```

`mo serve`'s lines have no timestamps, so read them with `-o cat` and drop
`-o cat` when you need the time. The agent guide's Run a service section
defines every line.

Health call, from any machine that holds `QUEUE_TOKEN`:

```sh
printf 'Authorization: Bearer %s\n' "$QUEUE_TOKEN" |
  curl -sS https://$HOST/mcp -H @- -H 'Content-Type: application/json' \
    -H 'MCP-Protocol-Version: 2025-11-25' \
    -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"stats","arguments":{}}}'
```

The token goes through `printf`, a shell builtin, so it never appears in a
process list or in shell history. A wrong token gets an empty reply; add
`-w '%{http_code}\n'` to see its status, 401.

## When it stops

| Exit | Restarted? | What to do |
| --- | --- | --- |
| killed by a signal, or 70 | yes, after 5 s | Nothing. Saved data is intact. Do the after-outage check. |
| 75 | yes, after 5 s | Another process, such as a second `mo serve` or a reader of the data folder, held `service.sqlite` for the whole 10-second wait. Nothing changed. Stop the other process. |
| 69 | no | Supervision stopped the service. Read the `failure:` lines, then follow the agent guide's Recover from a stop. |
| 1 | no | E1409 or a program error; restarting cannot help. Read the journal. An env file without a valid `QUEUE_TOKEN` is E1409 naming it. |
| systemd `Result=resources` | 5 tries, then stops | `/etc/task-queue/env` is missing or unreadable, so `mo` never ran. Restore the file, then `sudo systemctl reset-failed task-queue` and start it. |
| 2 | no | A mistake in `ExecStart`. Fix the unit. |
| 0 | no | Someone ran `systemctl stop`. Start it when they are done. |

Five starts within ten minutes stop systemd from trying (`start-limit-hit`);
`sudo systemctl reset-failed task-queue` clears it. Each exit 75 is a start, so a
store held for more than about a minute (five 10-second waits, 5 s apart) reaches
the limit too.

An empty `/var/lib/task-queue/service.sqlite` comes only from a kill during
the very first start. It never held data: delete it and start again. Never
delete a non-empty one.

After any outage, list the jobs that need a person:

```sh
for filter in Dead Leased; do
  printf 'Authorization: Bearer %s\n' "$QUEUE_TOKEN" |
    curl -sS https://$HOST/mcp -H @- -H 'Content-Type: application/json' \
      -H 'MCP-Protocol-Version: 2025-11-25' \
      -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"list","arguments":{"filter":{"case":"'$filter'"},"after":"0","limit":"100"}}}'
  echo
done
```

When a reply is a `response_limit` error, halve `limit`; when `next` is not
`None`, pass its value as `after` for the next page. A Leased job whose lease
outlived the outage returns to Ready by itself within a minute. A Dead job stays dead until someone resubmits its work.

## Why an idle queue still writes

The `expire` timer runs every minute and each run is a saved message, as is
every `lease` call, even one that finds no job. The data folder grows slowly
with no agent traffic. That is expected.

## Clients

Each client machine keeps the token in a file its shell reads, never in a
client's config. On the client:

```sh
install -d -m 0700 ~/.config/task-queue
ssh $HOST 'sudo cat /etc/task-queue/env' | sed 's/^/export /' |
  (umask 077; cat > ~/.config/task-queue/env.sh)
echo '[ -r ~/.config/task-queue/env.sh ] && . ~/.config/task-queue/env.sh' >> ~/.zshenv
```

A client that cannot SSH to the queue VM gets the file through one that can.
Run this on the one that can, with `CLIENT` naming the other:

```sh
ssh $HOST 'sudo cat /etc/task-queue/env' | ssh CLIENT 'install -d -m 0700 ~/.config/task-queue;
  sed "s/^/export /" | (umask 077; cat > ~/.config/task-queue/env.sh);
  echo "[ -r ~/.config/task-queue/env.sh ] && . ~/.config/task-queue/env.sh" >> ~/.zshenv'
```

On a client whose shell is bash, put that last line in `~/.bashrc` instead.
Every shell on the client, including an agent's, then holds the token, so never
print the environment (`env`, `printenv`, `set`) in an agent session.
Then start a new shell and add the server. Both lines name the variable, and
the stored config keeps the name, not the value.

```sh
claude mcp add --transport http --scope user task-queue https://$HOST/mcp \
  --header 'Authorization: Bearer ${QUEUE_TOKEN}'
codex mcp add task-queue --url https://$HOST/mcp --bearer-token-env-var QUEUE_TOKEN
```

`claude mcp get task-queue` shows `${QUEUE_TOKEN}`, and `claude mcp list`
connects to the queue. Codex needs the tool approved before `codex exec` can
call it, as with `-c 'mcp_servers.task-queue.tools.stats.approval_mode="approve"'`.

## Rotate the token

Generate a new token with Install step 4, `sudo systemctl restart task-queue`,
then on every client repeat the Clients section's first two commands, or the
relay, without the line that edits `~/.zshenv`, and start new shells. Threads
that hold the old token get 401 until they restart.

## Upgrade

A queue started with `--upgrade-token-env QUEUE_UPGRADE --approval-token-env
QUEUE_APPROVAL` accepts a live upgrade over loopback on its host without stopping.
Both are secrets of at least 16 bytes, different from each other and from the queue
token; `mo serve` refuses to start with the first and without the second (E1434).
The upgrade credential is kept in the environment file. The approval credential is
Robert's: it goes in a root-only file of its own, never in the file clients copy and
never on a machine where agents run, and the unit gains a second
`EnvironmentFile=/etc/task-queue/approval` line:

```sh
sudo sh -c 'umask 077; python3 -c "import secrets; print(\"QUEUE_APPROVAL=\" + secrets.token_urlsafe(32))" > /etc/task-queue/approval'
```

Robert lists the active program's items, approves them and sets the review setting
from his own login on the host, with `mo review` and one of `--items`,
`--base PROGRAM --approve DIGEST...` or `--setting everything|intent`. Approvals and
the setting do not yet gate an upgrade.

```sh
sudo sh -c '. /etc/task-queue/env; . /etc/task-queue/approval; export QUEUE_TOKEN QUEUE_APPROVAL
  exec "$1" review --url http://127.0.0.1:8000/mcp --token-env QUEUE_TOKEN \
  --approval-token-env QUEUE_APPROVAL --items' sh /opt/mo/$SHA/build/gcc/debug/mo
```

An agent pushes with the service token and the upgrade credential:

```sh
mo push NEW/main.mo --url http://127.0.0.1:8000/mcp --base DIGEST \
  --token-env QUEUE_TOKEN --upgrade-token-env QUEUE_UPGRADE [--migrate Module.function]
```

`DIGEST` is the active program's source digest (`base_source` from
`mo check NEW/main.mo --upgrade-from OLD/main.mo --json`). Exit 0 means the queue
runs the new program; repeating the push reports `already active`. A refusal leaves
the old program serving. A migration the live budget refuses can still use the
checked restart below. With an upgrade-capable unit, `ExecStart` names only the
folder (`mo serve --data /var/lib/task-queue --port 8000 ...`), so a restart runs
whichever program was last activated.

The deployed queue above is retired. For a queue running an upgrade-capable build,
a stopped-service checked restart on its host also works. Build the new pinned tree with Install step 2,
keep the old executable and source, stop the unit, and copy the whole stopped
folder (including any journal) before changing it.

Check the candidate against the old source with the new executable:

```sh
/opt/mo/$SHA/build/gcc/debug/mo check NEW/main.mo --upgrade-from /var/lib/task-queue
```

If the state schema changes, add `--migrate Module.function`. The function takes
one generated `UpgradeOld` state and returns the new state, without capabilities
or type parameters. This command checks types; it does not run the migration
against the saved queue.
The stopped folder supplies the recorded source and generated declarations from
any prior migration; the check leaves it unchanged. A standalone old source file
can also be used as `--upgrade-from OLD/main.mo` when it needs no retained context.

For the first start of the new version, set the unit's `ExecStart` to the new
executable and candidate source with `--upgrade`, retaining its port, data folder,
allowed host and environment file. Include `--migrate Module.function` if needed.
Run `sudo systemctl daemon-reload` and start the unit. A successful checked restart
validates the migrated state and commits it with the recorded program. A migration
refusal leaves the previous program and state authoritative; inspect the refusal
instead of repeatedly restarting the unit.

After success, remove the source argument, `--upgrade` and `--migrate` from
`ExecStart`, leaving `mo serve --data /var/lib/task-queue --port 8000` and the
existing allowed-host option. Reload the unit. Later starts compile the recorded
source and its retained generated declaration context; they do not rerun the
migration. Keep `RestartPreventExitStatus=1 2 69` so compilation or migration
refusals do not loop.

An older folder with no program record is adopted on an exact image match.
`--adopt` permits a different image only through verified replay; a report of
zero verified messages is not evidence that changed handlers preserve behavior.
If checked restart cannot accept the candidate, use the agent guide's fresh-folder
recovery procedure: export needed data through the old program's tools, start the
new program on an empty folder and import through its tools. Keep the old tree
and folder. Live swapping and streaming notifications are implemented in the
upgrade-capable build described above. The [run record](../status/phase5-run.md)
records the drill, the pause series and the agent evaluation, which passed on one
frozen build and shipped in v0.2.0. A work-budget refusal
is not a deadline guarantee.

## Drill

The drill proves the restart policy without touching the queue's data. It has
its own program, token, data folder and port. Run it after any change to the
unit's restart settings.

```sh
sudo install -d -m 0755 /opt/mo/drill
sudo tee /opt/mo/drill/main.mo >/dev/null <<'EOF'
module Main

service
  state Int = 0
  tools halt
  token from env "QUEUE_TOKEN"
end

## Always stops.
fn halt(_state: Int, _now: Time) : Text
  stop "halted on purpose"
end
EOF
sudo install -d -m 0700 -o taskqueue -g taskqueue /var/lib/task-queue-drill
sudo sh -c 'umask 077; python3 -c "import secrets; print(\"QUEUE_TOKEN=\" + secrets.token_urlsafe(32))" > /etc/task-queue/drill.env'
sed -e 's|^Description=.*|Description=Mo task queue restart drill|' \
    -e 's|^EnvironmentFile=.*|EnvironmentFile=-/etc/task-queue/drill.env|' \
    -e "s|^ExecStart=.*|ExecStart=/opt/mo/$SHA/build/gcc/debug/mo serve /opt/mo/drill/main.mo --port 8001 --data /var/lib/task-queue-drill|" \
    -e 's|^ReadWritePaths=.*|ReadWritePaths=/var/lib/task-queue-drill|' \
    -e '/^\[Install\]/,$d' /etc/systemd/system/task-queue.service |
  sudo tee /etc/systemd/system/task-queue-drill.service >/dev/null
sudo systemctl daemon-reload
```

A supervised stop (expect exit 69, `NRestarts=0`, and a `stop:` line). Five
failures within a minute restart the service and three restarts stop it, so
call `halt` until the unit is no longer active:

```sh
sudo systemctl start task-queue-drill
sudo sh -c '. /etc/task-queue/drill.env
  while systemctl is-active --quiet task-queue-drill; do
    printf "Authorization: Bearer %s\n" "$QUEUE_TOKEN" |
      curl -s -o /dev/null http://127.0.0.1:8001/mcp -H @- \
        -H "Content-Type: application/json" -H "MCP-Protocol-Version: 2025-11-25" \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"halt\",\"arguments\":{}}}"
    sleep 0.2
  done'
systemctl show task-queue-drill -p ActiveState -p ExecMainStatus -p NRestarts
journalctl -o cat -u task-queue-drill | grep '^mo serve: stop:'
```

A startup refusal (expect exit 1 with E1409, `NRestarts=0`):

```sh
sudo mv /etc/task-queue/drill.env /etc/task-queue/drill.env.off
sudo systemctl reset-failed task-queue-drill
sudo systemctl start task-queue-drill; sleep 10
systemctl show task-queue-drill -p ActiveState -p ExecMainStatus -p NRestarts
sudo mv /etc/task-queue/drill.env.off /etc/task-queue/drill.env
```

A crash, on the queue itself (expect active again within 5 s, `NRestarts=1`,
and every job still listed):

```sh
sudo systemctl kill --kill-whom=main -s SIGKILL task-queue; sleep 8
systemctl show task-queue -p ActiveState -p NRestarts
```

A crash that exits 70, such as a failed save, with the queue's own restart
settings (expect `ExecMainStatus=70` and `NRestarts=5`: systemd restarts it
until the start limit, then leaves it failed):

```sh
sed -e 's|^Description=.*|Description=Mo task queue exit-70 drill|' \
    -e 's|^ExecStart=.*|ExecStart=/bin/sh -c "exit 70"|' \
    -e '/^\[Install\]/,$d' /etc/systemd/system/task-queue.service |
  sudo tee /etc/systemd/system/task-queue-exit70.service >/dev/null
sudo systemctl daemon-reload
sudo systemctl start task-queue-exit70; sleep 40
systemctl show task-queue-exit70 -p ActiveState -p Result -p NRestarts -p ExecMainStatus
sudo systemctl reset-failed task-queue-exit70
sudo rm /etc/systemd/system/task-queue-exit70.service
sudo systemctl daemon-reload
```

A deliberate stop (expect exit 0 and inactive, with no restart), then start it
again:

```sh
sudo systemctl stop task-queue; sleep 8
systemctl show task-queue -p ActiveState -p ExecMainStatus -p NRestarts
sudo systemctl start task-queue
```

Remove the drill when it is done:

```sh
sudo systemctl reset-failed task-queue-drill
sudo rm -rf /etc/systemd/system/task-queue-drill.service /etc/task-queue/drill.env \
  /var/lib/task-queue-drill /opt/mo/drill
sudo systemctl daemon-reload
```

## Phase 4 operator workflow

The deployment above remains retired. These commands describe diagnosis for a service
installed with that layout using a Phase 4 build. Keep its exact `/opt/mo/$SHA` tree and
executable for as long as its saved data or failure records may be needed.

Authenticated MCP clients can call `mo-overview`, `mo-state`, `mo-mailbox`, `mo-timers`,
`mo-memory`, `mo-supervisor`, `mo-log` and `mo-failures`. The service token also grants access
to private state. Views are bounded and can be truncated; separate calls are not one atomic
snapshot. Committed history covers only the tail since the last snapshot.

Fetch on the service host over loopback, while the service runs. Read the token from its
protected environment file; the command takes the variable’s name, not its value. Choose an
output path that does not already exist:

```sh
sudo sh -c '. /etc/task-queue/env; export QUEUE_TOKEN
  exec "$1" fetch http://127.0.0.1:8000/mcp --export state --out "$2" --token-env QUEUE_TOKEN' \
  sh /opt/mo/$SHA/build/gcc/debug/mo /var/lib/task-queue-state.moex
```

The artifact is mode 0600. `mo fetch` accepts only HTTP loopback URLs, verifies the export,
and does not use the public HTTPS proxy. For a retained failure, replace `--export state`
with `--export failure ID`. Keep artifacts private.

Stop the service before reading its data folder. Run as its owner. If copying data for
analysis, copy the whole stopped folder, including any journal, never the database alone:

```sh
sudo systemctl stop task-queue
sudo -u taskqueue /opt/mo/$SHA/build/gcc/debug/mo history /var/lib/task-queue
sudo -u taskqueue /opt/mo/$SHA/build/gcc/debug/mo replay \
  /opt/mo/$SHA/services/task-queue/main.mo --failure ID --data /var/lib/task-queue
```

Replace `ID` with a retained failure id. If history reports recovery required, its owner can
run `history --recover /var/lib/task-queue`; this performs the journal rollback. Preserve the
original source exactly, including comments. `mo history` prints the recording build digest;
`sha256sum /opt/mo/*/build/gcc/debug/mo` finds the executable that matches it.

To query exported state, copy the source to a private analysis folder and add a pure function
that accepts the service state without changing its type. Run the recording executable as
`mo query COPY/main.mo FUNCTION --export /var/lib/task-queue-state.moex`, with an account that
can read the artifact. This runs offline and changes no live service state.

To check a proposed fix, edit a separate copy and add `--fix COPY/main.mo` to the replay
command. The original must reproduce before the revision can be `verified`. That verdict
covers the reconstructed case, not application correctness or live memory pressure: both
misses in the [diagnostic evaluation](../status/phase4-run.md) received `verified` but failed
hidden correctness tests. Run the application’s tests before considering deployment.
Restart the service when diagnosis of its stopped folder is finished.

### Push from a remote machine

Keep the host listener bound to loopback. On the operator machine, open an SSH
local forward (replace `HOST` and the host listener port `8080`):

```sh
ssh -N -o ExitOnForwardFailure=yes -L 127.0.0.1:8080:127.0.0.1:8080 HOST
```

Use the same local and host port: the server validates the HTTP Host authority.
If the local port must differ, start the host service with
`--allow-host 127.0.0.1:LOCAL_PORT` as well; an unlisted authority is refused with
HTTP 421. Leave that terminal running. In another terminal on the same operator machine,
use the service token and the distinct upgrade credential in environment variables:

```sh
mo push new/main.mo --url http://127.0.0.1:8080/mcp --base DIGEST \
  --token-env SERVICE_TOKEN --upgrade-token-env UPGRADE_TOKEN
```

`DIGEST` is the current canonical source digest, obtained from
`mo check new/main.mo --upgrade-from old/main.mo --json` (`base_source`) or a
credentialed stale-base refusal. Supply `--migrate Module.function` when needed.
For offline rehearsal also retain the original bundle and any MOCC compilation
context; neither a state export nor the service token supplies those source bytes.
The client validates the loopback URL before reading either credential. There is
no TLS client in this release: a public HTTPS endpoint is not a `mo push` target.
The SSH connection authenticates and encrypts the remote hop. The local-forward
listener binds only `127.0.0.1`; stop SSH after use.

Direct pushes and pushes through this forward use the same upload and activation
path and print identical acceptance/refusal text. Exit 0 means the candidate is
active (including already active), 1 means refusal or client failure, and 2 means
invalid usage, URL or base digest. A transport failure after commit may leave its
outcome unknown; repeat the same candidate to resolve it without reapplying a
migration. Transient no-effect rate refusals are retried within the command's
deadline. Sustained starvation can leave the slot busy until its idle expiry.

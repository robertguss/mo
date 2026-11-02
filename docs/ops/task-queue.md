# The task queue on exe.dev

Mo's task queue (`services/task-queue/main.mo`) runs under systemd on a
dedicated exe.dev VM, from a pinned build, behind the VM's public HTTPS route.
Agent threads reach it at `https://VM.exe.xyz/mcp` with a bearer token. The
current deployment and its drill results are in `docs/status/phase3-run.md`.

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
| 69 | no | Supervision stopped the service. Read the `failure:` lines, then follow the agent guide's Recover from a stop. |
| 1 | no | E1409 or a program error; restarting cannot help. Read the journal. An env file without a valid `QUEUE_TOKEN` is E1409 naming it. |
| systemd `Result=resources` | 5 tries, then stops | `/etc/task-queue/env` is missing or unreadable, so `mo` never ran. Restore the file, then `sudo systemctl reset-failed task-queue` and start it. |
| 2 | no | A mistake in `ExecStart`. Fix the unit. |
| 0 | no | Someone ran `systemctl stop`. Start it when they are done. |

Five starts within ten minutes stop systemd from trying (`start-limit-hit`);
`sudo systemctl reset-failed task-queue` clears it.

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

A new build is a new tree: run Install step 2 with the new `SHA`, change both
paths in the unit, `sudo systemctl daemon-reload` and restart. If the new build
refuses the data with E1409, its program changed. Point the unit back at the
old tree, read the jobs you need through `list`, then follow Recover from a
stop's fixed-program path with a new data folder. Keep the old tree and the old
folder.

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

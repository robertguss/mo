# Phase 4 evaluation

The evaluation of Phase 4, "agents can see inside it", follows `protocol.md`: fresh agent
sessions diagnose and fix seeded task-queue bugs, first with today's tools (the baseline),
then with the Phase 4 instruments. The rounds are compared with `compare.py`.

## The scenarios are private until the final round

A session can read this repository over the network. The scenario variants, whose
difference from the served queue is the bug, would give it away, so they live in a private
repository, `robertguss/mo-phase4-private`, checked out at mode 0700 beside this one. It
holds, under `variants/<name>/`, each variant's session files (`main.mo`, `job-queue.mo`,
`queue-tests.mo`), its bug report (`report.md`), its fixture (`fixture.json` and the rendered
`fixture-steps.mo`), its hidden tests with a reference fix and a broken fix, or its answer
key's rule, and `SHA256SUMS`; and `variants/README.md`, which says exactly what each variant
does. This repository holds the tools that read it, which take the variants' root as an
argument, and the plan (`docs/plans/2026-11-04-0315-feat-phase4-pilots-baseline-plan.md`),
which names each variant's family and shape and nothing more. The freeze will record every
private file by hash, and after the final round the private repository's history is
published and checked against it.

## The tools

- `fixture.py` builds a variant's data folder by replaying its step list into `mo serve`,
  renders the same step list into the module hidden tests replay into a simulation, runs
  the pre-session check against a running service, and computes a question's answer key.
- `grade.py` grades a session's folder: the variant's own tests over the session's, the
  hidden tests for a bug, the answer for a question. `grade.py --check` proves every variant
  sound: its reference fix passes, the variant and a broken fix each fail a hidden test,
  its report names no internals, and its hashes match.
- `harness.py` runs one session end to end as a separate unix user, `mo-session`: it checks
  the fixture, starts the service and the recording proxy, launches the client with the
  prompt (`prompt.md`, the variant's report filled in), ends it at the time limit, collects
  the records and grades them. `harness.py round` runs a round's seeded order one session
  at a time.
- `proxy.py` is the recording proxy between client and service. It writes every request and
  response, with bodies, status and timing, and never the token. `proxy.py match` checks
  that the proxy log and the client's own event stream hold the same tool calls, and
  `proxy.py hosts` lists any host other than the loopback address a session's commands
  addressed.
- `publish.py` copies a session's manifest and grade for this repository, leaving out the
  hidden tests' names and details.
- `scan.py` gates publication: it fails on any session token or provider credential, in any
  encoding the transports use, in the files selected for publication, and on any file it
  cannot read.
- `testdata/variants/synthetic/` is a small queue with the served queue's tools and one
  deliberate toy defect, unrelated to any scenario. `test_fixture.py`, `test_grade.py`,
  `test_proxy.py` and `test_harness.py` run on it, the last with `testdata/stub_client.py`
  standing in for both clients, and `ci/check.sh bench` runs them with `test_scan.py`; the six
  real variants are checked by hand (`grade.py --check MO PRIVATE_ROOT BUILT`), since CI
  cannot read the private repository. `test_harness.py` creates a session user of its own,
  `mo-session-test`, and needs passwordless sudo.

## Running sessions

Sessions run as the unix user `mo-session`, which can read nothing under the operator's home:
the home must be mode 0750 or 0700, and `harness.py setup` refuses to go on otherwise. The
pinned binaries live outside it, under `/opt/mo-phase4/bin`, owned by root: `mo`, `claude`,
and `codex` (a link into `/opt/mo-phase4/codex`, the copied standalone package). Since the
session user cannot write them, neither client can update itself, and the harness also runs
Claude Code with `DISABLE_AUTOUPDATER=1` and Codex with `check_for_update_on_startup=false`.
`setup` records each binary's SHA-256 and version in `~/.mo-phase4-harness/binaries.json`,
for the freeze. The harness's private state is in `~/.mo-phase4-harness` (mode 0700): the
session tokens (`tokens`) and the clients' stored credentials (`secrets`), both for the
scanner, and the snapshot of the session user's home that every session starts from.

`mo` reads its library from the `lib/` folder of the tree it was built in, a path compiled into
the binary, and an unreadable library folder is an error (E1102) for every `mo check`. A binary
built under the operator's home therefore fails every check the session makes, so the baseline is
built from an export of the tagged commit under `/opt/mo-phase4/baseline`, which is then made
root's:

```sh
sudo install -d -m 0755 -o root -g root /opt/mo-phase4
sudo install -d -m 0755 -o "$USER" -g "$USER" /opt/mo-phase4/baseline
git archive phase4-baseline | tar -x -C /opt/mo-phase4/baseline
(cd /opt/mo-phase4/baseline && make CC=gcc build/gcc/debug/mo)
sudo chown -R root:root /opt/mo-phase4/baseline && sudo chmod -R go-w,a+rX /opt/mo-phase4/baseline
MO=/opt/mo-phase4/baseline/build/gcc/debug/mo
```

```sh
H=learnability/phase4/harness.py
python3 $H setup "$MO" "$(command -v claude)" "$(command -v codex)"
sudo -u mo-session env -i HOME=/home/mo-session PATH=/opt/mo-phase4/bin:/usr/bin:/bin \
    DISABLE_AUTOUPDATER=1 TERM=$TERM bash   # log in once, in a shell as the session user:
#   claude                      (log in, then /exit)
#   codex login --device-auth   (the plain login waits for a browser on this machine)
python3 $H setup "$MO" "$(command -v claude)" "$(command -v codex)"   # again: snapshot the logins
python3 $H build /opt/mo-phase4/bin/mo ~/mo-phase4-private ~/phase4-builds/ROUND
python3 $H run ~/mo-phase4-private/variants/NAME ~/phase4-builds/ROUND/NAME claude OUT
python3 $H order SEED NAME... > order.tsv
python3 $H round order.tsv ~/mo-phase4-private ~/phase4-builds/ROUND OUT
python3 learnability/phase4/scan.py ~/.mo-phase4-harness/tokens ~/.mo-phase4-harness/secrets \
    -- FILES...
```

A session's records, in its OUT folder (mode 0700): `stream.jsonl` (the client's events),
`proxy.jsonl`, `service.log`, `start-data/`, `task-queue/` (the working folder as the session
left it), `prompt.txt`, `manifest.json` and `grade.json`. `harness.py reset` cleans up after a
harness that did not finish. The harness's docstring says exactly what a session gets and
what a run does, in order.

Only a session's `manifest.json` and `grade.json` are committed here, through `publish.py OUT
DEST`, which replaces each failed hidden test's name and detail in the grade with one line, since
the hidden tests stay private until the final round. The scanner reads DEST before the commit.
The pilots' record, with the rule that decides when a variant is ready to freeze, is
`pilot/ledger.md`.
- `preflight/` holds the experiments that sized the design; its README says what each found.
- `live-views/` holds the development evidence for the authenticated live views: client
  routing, local checks and process sizing. Its README says what each run showed.

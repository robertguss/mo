# Phase 4 preflight experiments

The experiments `docs/design/introspection.md` and `learnability/phase4/protocol.md` rest on. Each
section gives the command, the inputs, the result and what the design takes from it. Raw results
are in `results/`.

**Environment for every run below:** 2026-11-04, Linux 6.12 x86-64; `mo` built by `make CC=gcc`
from commit `0b15ebc4` (`build/gcc/debug/mo`, SHA-256 `5bd005aa…bc87213`), gcc 13.3.0; Python 3.14.
The tokens are fixed canaries that name themselves (`Phase4SizingCanary` and the like), not
credentials.

**Predictions.** None of these runs had a written prediction before it ran. They are feasibility
measurements, not claims about what Mo guarantees; the claims they lead to are predicted in the
design's verification section and in the protocol before their own checks run.

## Largest export

`sizing.py` drives a real `mo serve` over loopback and reads the stopped data folder with
SQLite's read-only mode.

```sh
python3 learnability/phase4/preflight/sizing.py build/gcc/debug/mo queue      # 5,000 jobs of 2,048 bytes
python3 learnability/phase4/preflight/sizing.py build/gcc/debug/mo grow 150   # 150 KiB a call
```

| Run | Largest state reached | Snapshot record | Peak resident | Last updates |
| --- | --- | --- | --- | --- |
| Task queue at its limits: 5,000 jobs of 2,048 bytes, 500 of them leased, head sequence 5,513 | 10,464,255-byte snapshot at sequence 5,500 | 10.0 MiB | 108 MiB (110,604 KiB) | 0.14 to 0.22 s each |
| `grow/main.mo`, 150 KiB of text a call | 109 calls, 16,742,400 bytes of text; call 110 refused with `memory_limit` and counted as a handler failure | 15,360,473 bytes at sequence 100 | 120 MiB (122,752 KiB) | 0.12 to 0.14 s |
| `grow/main.mo`, 1 MiB a call (first run) | 15 calls; call 16 refused | not reached (sequence 0 only) | 104 MiB | 0.10 to 0.14 s |
| `grow/main.mo`, empty (a run whose every call was refused for a wrongly typed argument) | — | 73 bytes | 54 MiB (55,140 KiB) | — |

What the design takes from it:

- Every legal state's encoding fits in `MO_LIMIT_SAVED_BYTES` (16 MiB), because the runtime refuses
  to accept a state it cannot save. An export of a state is at most that plus its header, so pins
  are sized from it. The task queue's own ceiling is 10 MiB.
- Today's process already peaks near 120 MiB resident at the largest state, before any Phase 4
  memory. The design's sizing adds the encoded current state, the pins and capture buffers on top.
- Each update at a large state takes about 0.15 s, most of it the saved copy the runtime makes of
  every new state.
- At the queue's limits, `list` with `limit` 10 returned a 390-byte reply: ten 2 KiB payloads do
  not fit one reply, and the tool's own advice is to halve the limit. A state view of the same
  queue cannot fit one reply either, which is why the design truncates views and exports for the
  whole state.
- Not measured, because it needs code that does not exist yet: the heap's own peak and the codec's
  scratch peak inside the process (`mo_heap_peak`, `mo_heap_scratch_peak` are not reachable from
  `mo serve`), and the cost of pins. The operator tools' implementation repeats this experiment
  with those numbers.

## Queries from another program

```sh
python3 learnability/phase4/preflight/query_prototype.py build/gcc/debug/mo
```

`query_prototype.py` fills a copy of the task queue past its first snapshot (105 jobs, one
leased), then starts variant programs on copies of its data folder. Today every record's image
key is derived from the whole program, so another program is refused before any value is decoded.
The prototype rewrites each record's image key to the variant's own key, recomputing the record
checksum, so that only the saved schema comparison and handler replay decide.

| Variant | Change | Loaded | Its stats equal the original's |
| --- | --- | --- | --- |
| `same` | none (checks the rewrite itself) | yes | yes |
| `private-query` | an exposed pure function `next_id_of` added to `JobQueue`, the state's defining module, reading its private `next_id` | yes | yes |
| `imported-query` | a new module `Query` reading the state through `JobQueue.jobs`, used by a new read-only tool `ready` (which answered 104) | yes | yes |
| `changed-schema` | the state record's field `next_id` renamed | **refused**: E1409, "saved service data is damaged or was written by a different program" | — |
| `no-rewrite` | `private-query` without the rewrite | **refused**: the same E1409 | — |

What the design takes from it: saved values carry module-qualified nominal names and full
schemas, so a different compiled program that keeps the state's type decodes it with today's
decoder, whether the query lives in the defining module or imports its exposed accessors, and a
schema change is refused. What it does not show: that a query program whose entry module is the
query itself (not `Main`) names modules the same way. `mo` names modules by their path from the
program's root folder, so a query file in the service's folder keeps the same names; the
query command's tests must cover a query entry. The refusal message today cannot say which check
failed, which is why the design separates program, build and schema identities.

## What survives a snapshot

```sh
python3 learnability/phase4/preflight/history_trace.py build/gcc/debug/mo
```

One job is submitted, then leased and failed three times until it is dead, then 120 more jobs are
submitted, and the queue is stopped.

| What was asked | Answer |
| --- | --- |
| The live view of the job before stopping | attempts 3, state `Dead` |
| Snapshot sequence in the stopped folder | 100 |
| Log tail | sequences 101 to 128, every one a `submit` |
| Any `lease` or `fail` in the tail | no |

What the design takes from it: after a snapshot, the folder shows that the job died and how many
attempts it had, and nothing of the calls that killed it or when. v0.1 keeps the post-snapshot
tail and relies on failure records, which snapshots never remove, for failures, and on current
state for wrong replies. The evaluation's scenarios are chosen within that limit.

## Client names and approvals

```sh
python3 learnability/phase4/preflight/client_probe.py build/gcc/debug/mo OUT codex
python3 learnability/phase4/preflight/client_probe.py build/gcc/debug/mo OUT codex-approve claude
```

`client_probe.py` starts `mo serve` on `clients/main.mo`, whose author tools are `mo_state` and
`count`, behind a recording proxy that adds two operator tools, `mo-state` and `mo-overview` (both
with `readOnlyHint`), answers calls to them itself, and forwards everything else. Each client was
asked to call `mo-state`, `mo_state` and `mo-overview` by their server names and report the names
it saw. `results/clients*/traffic.jsonl` is the proxy's ordered record; `summary.json` joins it to
the clients' streams. The streams themselves are not committed: Claude Code's includes the local
machine's hook output.

| Client | Approval setting | Names the model saw | Calls that reached the server, in order |
| --- | --- | --- | --- |
| Codex 0.166.0 (`gpt-6.1-sol`, `xhigh`) | `approval_policy="never"` | `mcp__probe__count`, `mcp__probe__mo_overview`, `mcp__probe__mo_state_94338136a5cf`, `mcp__probe__mo_state_c5d0a7c9f588` | `mo-state`, `mo-overview`; the call to `mo_state` failed in the client: "MCP tool call requires approval, but approval policy is never" |
| Codex 0.166.0 | and `mcp_servers.probe.default_tools_approval_mode="approve"` | the same four | `mo-state`, `mo_state`, `mo-overview` |
| Claude Code 2.1.317 (`claude-opus-5-5`) | `--allowedTools 'mcp__probe__*'` | `mcp__probe__count`, `mcp__probe__mo-overview`, `mcp__probe__mo-state`, `mcp__probe__mo_state` | `mo-state`, `mo_state`, `mo-overview`, after one `ToolSearch` to load the deferred schemas |

Every call reached the tool it was meant for, and each reply's text matched (`operator tool
mo-state`, `author tool mo_state, count 0`, `operator tool mo-overview`). The proxy saw the
`initialize`, `notifications/initialized` and `tools/list` exchanges and every call, with its
status, in order, for each client.

What the design takes from it: Codex rewrites the hyphen and hashes the collision; Claude Code
keeps both names. Neither misroutes. Codex runs tools carrying `readOnlyHint` without approval and
refuses unannotated ones when it cannot ask, so the evaluation must set the approval mode
explicitly, the same way in both rounds. A recording proxy gives a complete ordered record of the
traffic independent of either client.

## Host and build access

Every run above was an agent-side process on the service's own machine, as the protocol's sessions
will be. Each compiled the program from its source with the pinned `mo`, reached the service on
`127.0.0.1`, and read a stopped data folder as the user that owns it: `mo serve` creates
`service.sqlite` with mode 0600, and SQLite's read-only open needed nothing more. The two clients
did the same from an empty working folder. The fetch command does not exist yet, so its loopback
connection is shown by the clients' and scripts' own loopback calls, not by fetch itself.

## Not run here

- **Amp.** Earlier rounds ran as Amp threads. Amp is not installed on this machine, so its MCP
  handling was not probed.
- **A remote host.** The Phase 3 host was deleted on 2026-11-03; the protocol runs sessions on the
  service's own machine instead.
- **Model availability over time.** The probe shows which names the clients served on one day.
  Nothing here shows a name keeps serving the same model; the protocol records that limit.

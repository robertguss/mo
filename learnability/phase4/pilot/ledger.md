# Phase 4 pilots: the calibration ledger

Pilots decide whether each of the six scenario variants is solvable with today's tools but not
trivial, before anything is frozen. They measure nothing: no pilot counts toward the baseline or
the final round. This ledger keeps every pilot session in the order it ran, kept or not, with
its verdict and anything it changed. What a change did to a variant is written in the private
repository's notes, never here; this ledger names the change and the note.

## The rule

- Pilots run with the baseline `mo` and nothing from Phase 4, one session at a time, and stop
  at 30 sessions, or sooner once every variant is calibrated.
- The first two pilots are `shared-briefs`, one on each client, as the end-to-end check of the
  harness with real clients. After them, one pilot per variant per client, alternating
  clients.
- A wrong-state or failure variant is calibrated once a pilot has solved it at least once and
  failed it at least once, with at least one session on each client.
- A question variant is calibrated once at least one session on each client has run and its
  answer key has been checked against a built folder (`grade.py --check MO ROOT BUILT`). No
  pilot has to solve it: a baseline near zero there is the predicted result.
- After a variant's third pilot: solved in all three, it is hardened and piloted again; a
  wrong-state or failure variant solved in none is replaced within its family.
- A change to a variant is followed by `grade.py --check` and `--freeze` before its next pilot,
  and restarts that variant's count. The table below keeps the earlier sessions.
- Pilots may change a variant's defect, data, bug report, hidden tests and answer key, the time
  limit and the client settings. They may not change the three families, the number of
  variants, the session counts, the clients or the comparison.
- A variant still uncalibrated at the cap is frozen as it stands and named as such in the
  freeze.

## What the first two pilots check

The harness was tested only with a stub client, so the two `shared-briefs` pilots also check,
from each session's records:

- Codex honours `-c web_search="disabled"`: its stream holds no web search.
- The host check reads Codex's commands through their `bash -lc` wrapping: a command that
  addresses the service shows `127.0.0.1` and nothing else.
- The record match pairs every real tool call in each client's stream with the proxy log.
- The manifest's first assistant event is found for each client, and the grader reads each
  client's last message for its `ANSWER:` line.

The host check reads the commands as written, so a script written to a file and then run is not
checked. Robert reads each pilot's commands for that.

## Setup

- `mo`: built from `main` at `87948db24aa72c62fb5a05d72fbc1b1c6c2ad211`, which the annotated
  tag `phase4-baseline` names (pushed 4 Nov 2026). It has no source change since Phase 3.
  Exported with `git archive` to `/opt/mo-phase4/baseline` and built there with
  `make CC=gcc build/gcc/debug/mo` (gcc 13.3.0, Ubuntu 24.04) on 4 Nov 2026, so that its
  library path, `/opt/mo-phase4/baseline/lib`, can be read by the session user (see
  `../README.md`). The tree is root's.
  - SHA-256 `af407b466acfe13cfe7438fff625aecd5507bba8998ef866cfc4b33a54342640`, installed as
    `/opt/mo-phase4/bin/mo`.
  - Embedded guide (`docs/agent-guide.md`, and `mo guide`'s output) SHA-256
    `da951023406d083d5e717a7f9f7ba94108882c51b7a162f37e5ad2aa47d2ce7f`.
  - Embedded reference (`docs/reference/README.md`, and `mo guide --reference`'s output)
    SHA-256 `05ea0186022c6728781b7dc70858b13852d667eaf32a7aafb69e3d60c68dc1f9`.
- Clients, installed under `/opt/mo-phase4/bin` by `harness.py setup`:
  - Claude Code 2.1.317, SHA-256
    `eb224419e344575faa4a1f39b290b649425a10cd5ee2ef1d003480cd9ae57eb0`.
  - Codex CLI 0.166.0 (standalone, x86_64 musl), SHA-256
    `b8a11fedf24b2dfb59a70c9e52b030f9a508dcd49cd2dd0d41956f4c785156b8`.
- Logins: Robert logged both clients in as `mo-session` on 4 Nov 2026 (Claude Code with the
  claude.ai subscription, Codex with ChatGPT through `--device-auth`). The login shell had no
  `DISABLE_AUTOUPDATER=1`, so Claude Code installed 2.1.318 into the session user's home; that
  copy was deleted and the snapshot taken again, so every session starts with the pinned
  2.1.317 alone. The login steps now set the variable.
- Variants: `robertguss/mo-phase4-private` at `1331ded`.
- Data folders: built with `harness.py build` and the baseline `mo` into
  `~/phase4-builds/pilots-1` on 4 Nov 2026; all six built, `expire-one-per-tick` in 32 minutes
  for its lapse, the others in 1 to 3. `grade.py --check /opt/mo-phase4/bin/mo
  ~/mo-phase4-private ~/phase4-builds/pilots-1` then passed for all six: each reference fix
  passes, each defect variant and its broken fix fail a hidden test, both answer keys reproduce
  from the built folders, and every `SHA256SUMS` is current.
- Time limit: 3600 seconds.

## Sessions

Each session's `manifest.json` and `grade.json` are in `NN-VARIANT-CLIENT/`, copied by
`../publish.py` and passed by `../scan.py`. Its stream, proxy log, service log and folders stay
in Robert's private tree. Grade is the harness's: the grader, the record match and the host
check together. Verdict is Robert's reading of the session. Minutes are the manifest's
`seconds`. Records are the record match.

| # | Date | Variant | Client | Grade | Verdict | Minutes | Records | Changed |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 4 Nov 2026 | `shared-briefs` | Claude Code | success | Solved, by reading the start state instead of the service: it decoded the saved snapshot from the read-only data copy and confirmed with one `list` call. A sign that `shared-briefs` may be trivial as designed; judged after its third pilot. | 1.1 | 2 of 2 matched | nothing |

## Changes

| After pilot | Variant | What changed | Private note |
| --- | --- | --- | --- |

## Summary

Solves and failures are counted since the variant's last change.

| Variant | Family | Claude sessions | Codex sessions | Solved | Failed | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| `expire-one-per-tick` | Wrong state | 0 | 0 | 0 | 0 | not piloted |
| `expire-kills-third-attempt` | Wrong state | 0 | 0 | 0 | 0 | not piloted |
| `shared-briefs` | Question, state | 1 | 0 | 1 | 0 | piloting |
| `who-killed-the-job` | Question, history | 0 | 0 | 0 | 0 | not piloted |
| `list-pasted-brief` | Failure, defect | 0 | 0 | 0 | 0 | not piloted |
| `stats-quadratic` | Failure, work limit | 0 | 0 | 0 | 0 | not piloted |

Pilot sessions used: 1 of 30.

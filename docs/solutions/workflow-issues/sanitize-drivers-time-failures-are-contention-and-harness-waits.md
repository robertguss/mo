---
title: A sanitize-build time failure is contention or a harness wait, not a product limit
date: 2026-11-19
category: workflow-issues
module: ci
problem_type: workflow_issue
component: testing_framework
severity: high
applies_when:
  - The nightly's sanitize drivers job fails a check with a time limit and the debug builds pass
  - A refusal reads "the checks did not finish inside their limits" (E1432) or "another upload slot is open" (E1415)
  - A Python driver waits on a mo process with its own timeout or proxies its traffic
  - A test asserts that a command fails without checking which failure it was
tags: [nightly, sanitize, asan, flaky, timeout, harness, kill-loop, upgrades, check-sh]
---

# A sanitize-build time failure is contention or a harness wait, not a product limit

## Context

From 11 to 19 November the nightly went red about every other run, always in the `drivers` group
on `build/clang/sanitize/mo`, never on the debug builds. The failures moved around: the kill loop's
push refused with E1432, the remote-upgrade check refused with E1415, a checked-restart service
never printed "listening", a hidden promise returned 1 with nothing on stderr, `mo test
lib/json.mo` passed its 30 s timeout. The same commit passed on another run. Splitting the nightly
into seven jobs (#377) did not stop it.

The first hypothesis was wrong. It said the upgrade check child (`mo test BUNDLE --bundle`) ran
near its CPU limit under ASan. Tracing every child's exit and its CPU time showed otherwise: the
task queue's check child uses about 44 s of CPU against `MO_LIMIT_CHECK_CPU_SECONDS` 120
(`src/base/limits.h:622`). What ran out was the 180 s wall limit (`src/base/limits.h:623`, enforced
in `src/host/compile.c:140`). It ran out because four ASan checks shared a 4-core runner, each with
a server, its children and a client.

## Guidance

**Measure before raising anything.** For each process with a deadline, get its CPU time and its
wall time, alone and under the job's load. Per-process CPU on this VM:

| | gcc debug | clang sanitize |
| --- | --- | --- |
| refused push in `check_upgrade_remote.py` | 9.6 s | 26 s |
| task-queue check child (kill loop) | about 9 s | about 44 s |
| `Answers.JobQueue.acknowledge.promise.1` | | 0.62 s of its 2 s |

ASan costs about 2.7 times per process. Contention is the second multiplier. With eight checks on
four cores, the slowest check child took 107 s of wall time to do 44 s of work.

**Lower the contention, keep the limits.** `ci/check.sh` takes `MO_SIDE_BY_SIDE`
(`ci/check.sh:78`), and the nightly sets it to 2 for the sanitize drivers job. The product limits
stay the same on every build. On the real runner the job went green three times out of three, and
it got faster: 40 to 47 minutes against 48 to 50, and the kill loop 354 to 461 s against 589 to
1,096 s.

**A harness wait must not be shorter than the client's.** `push()` gives `mo push` 120 s
(`ci/check_upgrade_live.py:288`). The remote check's two test proxies gave up after 60 s of
silence, so a slow commit was dropped mid-flight. The proxies now wait 120 s
(`ci/check_upgrade_remote.py:33`, `:78`). A service started for a checked restart had 60 s to start
listening, while the held run of the same restart gets 300 s; it now gets 300 s
(`ci/check_upgrade_live.py:168`).

**An expected failure must name its code.** The remote check asserted only
`refused.returncode == 1`. When the proxy dropped the commit, `mo push` exited 1 with "the service
did not answer the commit". The test took that for the expected E1421 refusal and went on. The
server was still committing, so the next push hit E1415 and the failure was reported one step too
late, at a line that was not the cause. The assertion now requires `error E1421:`
(`ci/check_upgrade_remote.py:135`).

## Why This Matters

A wrong reading leads to a wrong fix. Raising `MO_LIMIT_CHECK_CPU_SECONDS` would have changed
nothing, because CPU was never close. Raising the product's wall limits for sanitizer builds would
have spread to the drivers' own timeouts: the kill loop gives a push 300 s, so a larger child
limit needs a larger driver limit, and so on. An assertion that accepts any failure turns a dropped
connection into a pass and moves the symptom to the next call.

## When to Apply

- A check fails only on the sanitize build. Compare its per-process CPU with gcc debug, then
  compare its wall time with its CPU time. A large gap between them means contention.
- E1432 with no detail means the child wrote nothing to stderr: a deadline, a CPU or kill signal,
  or a failure before `exec`. Every one of these gives the same message
  (`src/tools/upgrade.c:281`), so find out which one with a trace before you choose a fix.
- A new driver proxies, relays or waits on a mo process. Its wait must be at least the timeout of
  the client it serves.
- A test expects a command to fail. Assert the error code or message, not only the exit status.

## Examples

Reproduce the harness bug without load: copy the old remote check, set both proxy timeouts to 5 s,
and run it on the sanitize build. It fails exactly as the nightly did, with E1415 on the accepted
push. The same 5 s timeout on the fixed check fails on the refused push instead, with "the service
did not answer the commit", which is the real cause.

```python
# before: any exit 1 passes, including a dropped connection
assert refused.returncode == 1, refused
# after: only the refusal the test means
assert refused.returncode == 1 and refused.stderr.startswith(b"error E1421:"), (mode, refused)
```

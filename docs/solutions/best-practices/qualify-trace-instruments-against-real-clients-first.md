---
title: Qualify a trace-parsing instrument against the real client before building synthetic controls
date: 2026-11-10
category: best-practices
module: learnability/phase5/evaluation
problem_type: best_practice
component: testing_framework
severity: high
applies_when:
  - An instrument parses another program's real behaviour, such as syscall traces or client log formats
  - Its tests are hand-built fixtures written by the same author as the parser
  - A live run is expensive, rate-limited or irreplaceable, such as a scored evaluation slot
tags: [strace, capture, evaluation, real-client, fixtures, offline-replay]
---

# Qualify a trace-parsing instrument against the real client before building synthetic controls

## Context

The Phase 5 evaluation collector traces a pinned agent client with strace and grades the session
from that trace. Before any real client had run through it, the capture passed 237 focused tests.
The first real Codex and Claude development sessions then failed every evidence dimension, and
three declared development pairs were needed to find eight defects (pending in PR #321):

- Codex's own sandbox runs commands in bubblewrap user, PID and mount namespaces.
- A multi-threaded client backs up a config file with `copy_file_range`.
- strace prints a space before `<unfinished ...>`, so resumed calls failed exact matches.
- DNS over UDP hit a TCP-only rule.
- Single writes above strace's `-s` limit were truncated and aborted the whole parse.
- Bun writes stdout through a duplicate of descriptor 1.
- The proxy synthesizes a 502 reply whose digest was never recorded.
- Sender identity was compared by thread, and both clients read and write on different threads.

Every one of these is a fact about the client or the tracer. None could have come from a fixture,
because the fixtures encoded the author's model of the client, and the parser encoded the same model.

## Guidance

1. Run one bounded real-client session through the whole pipeline as the first test of the
   instrument, before writing negative controls. Declare it as development evidence and keep its
   sealed inputs.
2. When that session fails, diagnose from the sealed trace offline. Re-parse it with the fix
   applied and read the next failure. Do not spend another live run to find the next defect.
3. Turn each real-trace shape that broke the parser into a focused test, using the shape exactly
   as the tracer printed it.
4. Spend a new live run only when the fix changes what gets recorded, not how it is read.

## Why This Matters

In this work the first live pair found the namespace and `copy_file_range` failures. Each later
live pair stopped at its first failure, and offline re-parsing of its sealed trace then found
the rest without a model call. Two fixes changed what was recorded (the strace buffer size and the proxy's 502
digest), and only those needed a new run. Had the scored four-slot cohort been the first real
run, all four irreplaceable slots would have been instrument misses.

## When to Apply

- Before freezing any collector, grader or capture that will judge a real client.
- Whenever a parser's only inputs so far are fixtures its author wrote.

## Examples

Fixture shape that passed: `10 pipe2([3<pipe:[7]>, 4<pipe:[7]>], O_CLOEXEC) = 0`

Real shape that failed: `10 pipe2( <unfinished ...>` followed by
`10 <... pipe2 resumed>[36<pipe:[54493390]>, 37<pipe:[54493390]>], O_CLOEXEC) = 0`. The joined
arguments begin with a space, and the creation pattern anchored at `[`.

## Related

- `learnability/phase5/evaluation/integration-development.md` records each failed run and repair.

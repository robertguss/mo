# MO-160 part 2: handoff

PR #262 stays a draft and is not merge-ready: it needs the full gate on its final
head, Robert's decision on the supervisor-event finding, and his merge approval.
Robert is taking over this work in another process. The accompanying
`mo-160-part-2-handoff.tar.gz` preserves the original measurement harness, client
capture validator, raw results and earlier check logs. It excludes binaries,
installed dependencies and credentials. The GCC/drivers, socket/client and Round E
reports each have their own archive beside them in `docs/status/`.

## Implemented

- Native server construction, connection polling, service dispatch and bounded
  request/reply conversion, using the real/fake listener already published.
- Cached immutable catalog graphs and reserved allocation-independent fallbacks.
- Rate, supervision, busy, work, memory, store-failure and response-limit tests.
- Empty-body `Expect: 100-continue` handling: interim 100 then final parse error,
  without waiting for more client input or admitting a call.
- Bounded duplicated result rendering through `Mcp.tool_reply`.
- Round E: listener restart, retryable accept errors, pre-auth resource refusals
  and body-read coalescing ([report](mo-160-part-2-round-e.md)).
- Round F: native D329 ceiling pairs in `tests/test_protocol_bounds.c`, transport
  failure-path tests, `fuzz/fuzz_protocol_server.c`, `MO_LIMIT_MCP_RESPONSE_NODES`,
  the port range check and CONFIG status for a NULL authority.
- Agent guide, language reference, service design, learnings and conformance
  inventory changes. Round G corrected the docs and inventory only; it changed no
  product code or test behavior and has had no independent review.

## D329 measurements

The measured byte ceilings are header 1 KiB, decoded body 16 KiB, complete
response 32 KiB and encoded ingress 24 KiB including headers. Previously these
were 2 KiB, 256 KiB, 256 KiB and 512 KiB respectively. No memory/work allowance
or non-byte ceiling increased. The 64 KiB ingress attempt exhausted work;
32 KiB exceeded the 2x threshold; 24 KiB passed.

The original strict GCC 12.2 `-O2` matrix and all eight catalog modes exited zero,
with zero semantic or headroom failures. The GCC 13.3.0 rerun in the
[GCC/drivers report](mo-160-part-2-gcc-drivers.md) passed too. Round E's rerun,
which added the small-read body cases, passed all 31 matrix cases and all eight
catalog modes ([report](mo-160-part-2-round-e.md)). Round F changed source after
that rerun; no later D329 matrix run is recorded.

Bytewise pre-authentication used 482,877 work and 75,862 bytes. The final chunk
case used 15,709,453 work. Maximum request peak was 3,785,074 bytes. Eight retained
maximum replies had a conservative concurrent bound of 13,090,128 bytes versus
13,062,136 independently summed protocol-process peaks. Concurrent accounting is
a sum of active lifetime peak bounds, not a simultaneous-live RSS measurement.
Native scratch peaks can conservatively include earlier service activity.

Original node-ceiling payloads exceed the new body-byte ceiling and are rejected
before parsing; do not claim native admission of 16,384-node bodies. Pure
transport fixtures retain the exact node-boundary checks. The maximum type graph
passes as 32 one-tool pages. Byte-fitting Set/Map duplicates, unique controls,
depth 60/61, exact response/schema limits, chunking and state oracles pass.
Raw fake-clock `stage_us` is zero; separate monotonic stage and poll columns in
the TSV files contain actual timing. No numerical native-time ceiling is claimed.

## Clients

Claude Code 2.1.314 is the real client of record. It completed initialize, the
initialized notification, both catalog pages (32+1) and one real `read` tools/call,
and negotiated 2025-11-25. Its genuine initialize was 391 body bytes and 20 JSON
nodes, excluding keys. One successful read is the whole real-call coverage: no real
updating or error call is claimed. See the
[socket/client report](mo-160-part-2-socket-clients.md).

Amp is unavailable on this VM. Earlier Amp exchanges remain historical: on an
earlier native adapter build, two initialize, two initialized notifications and two
tools/list requests passed, with a 254-byte, 14-node initialize. Its tools/call was
account-gated: two `amp svc` attempts sent zero calls and reported
`amp svc is not enabled for your account`. No Amp tool-result acceptance is claimed.

The Python controls are synthetic, not real-client calls. The final socket controls
cover ten calls, ceiling pairs, the bad bearer token and the reserved fallbacks; all
pass in the socket/client report.

## Checks

GCC/drivers (`MAKEFLAGS=-j4 CI=1 bash ci/check.sh gcc drivers`) passed on
`e145ee32`, recorded by `9af1395b`, which adds only the report and its archive. It
also passed on the Round E and Round F heads.

Full gate on `ae4372e`, run on a separate Cursor cloud agent (`bc-1831b869`) on
30 October 2026, 3:43-4:08 PM ET:

`MAKEFLAGS=-j4 CI=1 bash ci/check.sh gcc clang sanitize drivers static lean`
exited 0 in 1043.6 s with no skips.

| Group | Result | Time | Notes |
|---|---|---:|---|
| gcc | Pass | 48 s | 57 unit tests |
| clang | Pass | 50 s | |
| sanitize | Pass | 166 s | clang ASan+UBSan |
| drivers | Pass | 411 s | plus 221 s shared rebuild |
| static | Pass | 15 s | grammar, deps, library-map, format, tidy |
| lean | Pass | 133 s | `lake build MoCore mocore`, 74 jobs plus differentials |

`ci/check.sh fuzz` exited 0 in 248.8 s: 29 libFuzzer targets, 30 smoke runs at
`FUZZ_SECONDS=5` including `fuzz_bytecode_io_large`, and no crash artifacts. CBMC
was not run; it was not in the requested gate.

Toolchain: elan 4.2.4, Lean 4.34.0 (pin v4.34.0), gcc 13.3.0, clang 18.1.3,
clang-format 14.0.6, clang-tidy 18.1.3, bison 3.8.2.

This gate predates Rounds E, F and G. Their later changes have not been through
clang, sanitize, static, lean or fuzz in any recorded run, and the 30th fuzz target,
`fuzz_protocol_server`, was added after the smoke above. A new full gate will run on
the final head.

## Reviews and open items

- A Claude review and a Cursor review of `ae4372e` found issues that Rounds E and F
  addressed. Round E also had its own fresh-context review.
- The supervisor-event logging finding is pending Robert's decision and is untouched.

## Continue in the receiving process

1. Check out the PR head and read the Round E report and the commits since `ae4372e`.
2. To repeat the strict optimized matrix, use the harness and adjusted runner in
   `mo-160-part-2-round-e.tar.gz`; change only the runner's checkout path.
3. Run `MAKEFLAGS=-j4 CI=1 PATH=/opt/elan/bin:$PATH bash ci/check.sh gcc clang sanitize drivers static lean`
   and `ci/check.sh fuzz` on the final head. Record every group's result and any skips.
4. Rebuild the native socket harness against the final sources if repeating
   client controls; provide a new disposable token. Do not reuse old binaries
   or claim account-gated real calls succeeded.
5. Update the PR with final results. Do not merge without Robert's approval.

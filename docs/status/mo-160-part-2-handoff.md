# MO-160 part 2: work-in-progress handoff

Robert is taking over this work in another process. PR #262 must stay a draft;
this snapshot is not merge-ready. No new implementation or full gate was started
for the handoff. The accompanying `mo-160-part-2-handoff.tar.gz` preserves the
measurement harness, client capture validator, raw results and earlier check logs.
It excludes binaries, installed dependencies and credentials.

## Implemented

- Native server construction, connection polling, service dispatch and bounded
  request/reply conversion, using the real/fake listener already published.
- Cached immutable catalog graphs and reserved allocation-independent fallbacks.
- Rate, supervision, busy, work, memory, store-failure and response-limit tests.
- Empty-body `Expect: 100-continue` handling: interim 100 then final parse error,
  without waiting for more client input or admitting a call.
- Bounded duplicated result rendering through `Mcp.tool_reply`.
- Agent guide, language reference, service design, learnings and conformance
  inventory changes. Documentation is written, not independently reviewed.

## D329 measurements

The final strict GCC 12.2 `-O2` matrix and all eight catalog modes exited zero,
with zero semantic or headroom failures. The before/after source manifests are
identical and match the handed-off implementation. The metadata records parent
commit `357d102577f8ca530a09b7181fb1c528ef24fc00`: measurements included the then
uncommitted source, not only that parent commit.

The measured byte ceilings are header 1 KiB, decoded body 16 KiB, complete
response 32 KiB and encoded ingress 24 KiB including headers. Previously these
were 2 KiB, 256 KiB, 256 KiB and 512 KiB respectively. No memory/work allowance
or non-byte ceiling increased. The 64 KiB ingress attempt exhausted work;
32 KiB exceeded the 2x threshold; 24 KiB passed.

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

Real Amp doctor/catalog exchanges passed on an earlier native adapter build:
two initialize, two initialized notifications and two tools/list requests.
Initialize was 254 bytes and 14 JSON nodes, excluding property keys; negotiated
version was 2025-11-25. Two `amp svc` attempts sent zero tools/call requests:
`amp svc is not enabled for your account`. Standalone Claude Code was absent.
No real-client tool-result acceptance is claimed.

Separate Python HTTP controls completed six calls covering read, update,
application rejection, handler failure and recovery. All eight schemas and six
structured results passed jsonschema 4.26.0 Draft202012Validator; text content
matched structuredContent. These are not real-client calls and do not validate
the final bounded renderer on a real socket. The old interop service was stopped
and its disposable credential removed at handoff.

## Checks and unfinished work

- Targeted server/fault tests and pure transport/lowering tests passed GCC,
  Clang and ASan/UBSan before handoff. Earlier scalar/composite checks also pass.
- The earlier full six-group attempt passed GCC, Clang and sanitizer groups on
  an in-flight snapshot, then failed the drivers conformance inventory.
  Inventory changes and its regression tests subsequently passed locally.
- The final full `gcc clang sanitize drivers static lean` gate has not run.
  Elan is available. No full group result is asserted for the WIP commit.
- Full fuzz smoke passed before the last bounded-render and byte-ceiling changes;
  it must be rerun. CBMC was not run. Missing-tool skips were not reported by the
  recorded six-group attempt; static and Lean were not reached after failure.
- Fresh independent code review has not started. Earlier design exploration is
  not review approval. Final docs and conformance mapping need that review too.
- Handoff-only checks are whitespace, source-manifest consistency, evidence
  credential exclusion, archive readability and local/remote head agreement.

## Continue in the receiving process

1. Check out the PR head and inspect the WIP diff and this evidence archive.
2. Extract the archive into `/tmp`; it contains `mo160-headroom.c` and
   `mo160-headroom-run.sh`. The script assumes this checkout is
   `/home/user/workspace/repo`; adjust only that path if needed. Run
   `bash /tmp/mo160-headroom-run.sh` to reproduce the strict optimized matrix.
3. Run `MAKEFLAGS=-j4 CI=1 PATH=/opt/elan/bin:$PATH bash ci/check.sh gcc clang sanitize drivers static lean`
   and final fuzz smoke. Record every group's result and any skips.
4. Rebuild the native socket harness against the final sources if repeating
   client controls; provide a new disposable token. Do not reuse old binaries
   or claim account-gated real calls succeeded.
5. Obtain a fresh independent review, address findings and rerun affected checks.
   Update the PR with final results. Do not merge without Robert's approval.

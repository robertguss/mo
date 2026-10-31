# MO-160 part 2: native sockets and clients, 30 October 2026

PR: https://github.com/robertguss/mo/pull/262 (draft; body and draft state unchanged).
Built source: `9af1395b6673d008a73b4231f182b13d5112f3f9`, confirmed clean and equal
to `origin/mo-160-part-2` after the initial fetch. This evidence commit changes
only this report and its archive. No product code, limits, schemas, tests or
code comments changed. No merge, rebase, force-push or Notion edit occurred.

## Build and capture

The archived `mo160-live.c` socket harness was rebuilt against this worktree's
sources with GCC 13.3.0, C17, `-O2 -g -pthread -Wall -Wextra -Werror`.
The runtime objects were freshly built in `build/mo160-socket` with the
repository's strict warning flags. Build and link both passed.

The disposable harness retains the archived add/read/reject/fail service and
adds the existing server-test large/grow/work examples and 26 read-only tools.
Its 33-tool catalog exercises pagination. All harness changes live in the
archive, outside product code and repository tests. The service uses the real
native listener on an ephemeral loopback port and an in-memory store.

A loopback HTTP capture forwarder records real Claude requests and native
responses. It forwards the bearer header, rewrites Host to the native authority,
and closes each forwarded connection. Claude receives the returned JSON through
the forwarder. SYNTHETIC Python controls connect directly to the native listener.
Raw HTTP request/response files and decoded captures are preserved. Only the
bearer token is redacted, with the same number of asterisks to preserve lengths.
Request/response JSON bodies are unredacted.

The archived `mo160-client-summary.py` validator is preserved. Its schema and
text/structured-result checks were adapted in `validate-captures.py` for current
Claude captures, the 33-tool catalog and the current SYNTHETIC controls, replacing
old Amp-specific counts/assertions. Offline validation passed with jsonschema
4.26.0 Draft202012Validator: all 66 input/output schemas, one real Claude result,
and ten SYNTHETIC results. Every text result decodes to its structuredContent,
and every reply preserves its request ID.

## Socket results

Every Python control in this table is **SYNTHETIC**, not real-client acceptance.
The real Claude coverage is listed separately below.

| SYNTHETIC case | Expected | Observed | Result |
|---|---|---|---|
| initialize | HTTP 200, protocol 2025-11-25 | Exact version returned | Pass |
| tools/list and cursor page | HTTP 200, 32+1 tools, final page without cursor | 33 distinct tools, pages 32+1 | Pass |
| read/add/read | Values 0, 7, 7 | Values 0, 7, 7; schema-valid results | Pass |
| application rejection | isError, application error denied | Exact error; state remains 7 | Pass |
| handler failure and recovery read | handler_failed, effect none, canary absent; state 7 | Exact error and recovery state 7 | Pass |
| bad bearer token | HTTP 401, empty body | HTTP 401, empty body | Pass |
| header block 1,024 B | HTTP 200 | HTTP 200 | Pass |
| header block 1,025 B | HTTP 431, empty body | HTTP 431, empty body | Pass |
| decoded body 16,384 B | HTTP 200 | HTTP 200 | Pass |
| decoded body 16,385 B | HTTP 413, empty body | HTTP 413, empty body | Pass |
| encoded ingress 24,576 B, 1,024 chunks | HTTP 200 | HTTP 200 | Pass |
| encoded ingress 24,577 B, 1,024 chunks | HTTP 413, empty body | HTTP 413, empty body | Pass |
| oversized read-only result | Reserved response_limit, effect none | Exact schema-valid fallback | Pass |
| oversized updating result | Reserved response_limit, effect applied | Exact schema-valid fallback; recovery read 8 | Pass |
| handler work exhaustion | Reserved work_limit, effect none | Exact schema-valid fallback; state remains 8 | Pass |

The three fallback calls use IDs containing 128 NUL characters (770 encoded
JSON-string bytes); responses preserve those IDs exactly. Fallback JSON bodies
are 1,156 / 1,162 / 1,170 B respectively. These checks exercise response-limit
and work-limit emission over real sockets. They do not claim socket-level
fault injection for every reserved error/effect variant or allocation-exhaustion
proofs. The handler-failure path also returned its fixed runtime error without
leaking its canary.

Measured byte ceilings remain header 1 KiB, decoded body 16 KiB, complete
JSON-RPC response 32 KiB and total encoded ingress 24 KiB including headers.
No ceiling changed. Oversized 32,768-character replies trigger the complete
response fallback because the duplicated content exceeds 32 KiB. This thread
did not repeat the D329 headroom matrix or establish a wall-time threshold.

## Real clients

| Client | Version | Available? | Exchanged | Result | Limitation |
|---|---|---|---|---|---|
| Claude Code | 2.1.314 | Yes, existing authentication worked non-interactively | initialize, initialized notification, two tools/list pages, one tools/call read | Exit 0; 33 tools listed, real result value 0 accepted | One successful read call; no real updating/error call claimed |
| Amp | Unavailable | No; command absent | None | Availability exception | Not installed; no installation or sign-in attempted |
| SYNTHETIC Python controls | Python 3.14.7 | Yes | Direct HTTP initialize/list and ten calls plus rejection controls | All final controls pass | Not real-client acceptance |

Claude's genuine initialize is **391 body bytes and 20 JSON value nodes**,
excluding object keys (39 nodes including keys). Both the request and response
select **2025-11-25**. ClientInfo identifies `claude-code`, version `2.1.314`.
Claude first sends a 508-byte `server/discover` probe for protocol 2026-07-28;
the native endpoint returns HTTP 400 with its supported-version message.
Claude then initializes successfully. Its optional GET receives HTTP 405;
Claude continues through both catalog pages and the tool call. Final-attempt
real traffic totals seven HTTP requests: one 400, four 200, one 202 and one 405.

The non-interactive invocation uses an ephemeral `--mcp-config` with the bearer
header, `--strict-mcp-config`, no built-in tools, and permission only for the MCP
read tool. It uses `--no-session-persistence`. No account gate or login prompt
was encountered or bypassed. The prompt asks Claude to list the tools and call
read once; its recorded output reports all 33 tools and the returned value 0.

Two earlier attempts each completed the same real Claude exchange. Their
SYNTHETIC follow-up omitted `MCP-Protocol-Version` and correctly received HTTP
400 on tools/list. The scratch-client header correction initially failed to
apply, then was applied and verified. The final attempt passes every control.
All three attempts and their cleanup records remain in the archive; only the
final attempt supplies the passing SYNTHETIC results above. No product defect
was found, and no implementation fix was needed.

## Cleanup and evidence

Each attempt generated a fresh random 64-byte hexadecimal token in a mode-0600
file. All three native services exited zero after SIGTERM. Connection probes
confirmed both the native and capture-forwarder ports closed after each run.
All token and MCP config files were deleted. Each run scanned its evidence for
its disposable token and found none. The archive excludes credentials,
executables and installed dependencies.

Evidence is retained under `/tmp/mo160-evidence` and committed as
`docs/status/mo-160-part-2-socket-clients.tar.gz` (137 files).
Archive SHA-256:
`0988621f534676171d4d4683490e8417bc0400710e16faf7cea8c156743f9b4d`.
It includes build logs, harness source/build script, control runner, archived
and adapted validators, raw HTTP bytes, decoded captures, client output,
validation summaries, cleanup records and source manifests. The before/after
SHA-256 manifests for tracked `src`, `runtime` and `lib` files match exactly.

No full check group ran in this socket/client-only thread: gcc, clang,
sanitize, drivers, static, lean, fuzz and cbmc are all **not run here**, with
no group-level pass or missing-tool skip claimed. The fresh GCC runtime/harness
build, socket controls and offline validation passed. Whitespace and archive
readability checks passed before commit. Broader gate and independent-review
work remain outside this thread. No new product blocker was found; Amp
availability and the limited real-call coverage above remain explicit.

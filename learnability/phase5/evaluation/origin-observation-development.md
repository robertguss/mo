# U2 origin observation: incomplete development investigation

2026-11-09. No scored sessions or model runs. U2 is not implemented or qualified.
This note does not establish that transparent attribution is infeasible.
U1 actual Codex serving-model proof remains a separate collection blocker.

Read the plan's R3, R7, KTD2 and U2, repository AGENTS, Phase5 proxy/tests,
collector launch/cleanup, Phase4 transport/matcher, and existing development
controls and runner notes. The isolated checkout has no graph index; direct
source reads were used after the coverage query reported that limitation.
No main/nightly checkout, index, branch, commit, push, PR, harness, runtime or
existing result was changed.

## Proof-first observations

Added `origin_controls.py`, a directly invoked development suite (deliberately
outside ordinary `test_*.py` discovery). Before any implementation change:

```
PYTHONDONTWRITEBYTECODE=1 python3 learnability/phase5/evaluation/origin_controls.py -v
Ran 6 tests in 0.004s
FAILED (failures=4)
```

Passed: echo-only command does not attest; unanswered upload does not attest.
Failed: push text without execution evidence, repeated RPC IDs with duplicate
arguments, unrelated HTTP beside push, and a fabricated origin field alongside
push text. Each failing case incorrectly returned `match: true`, with all
uploads placed in `command_origin` as `command: mo push`. Repeated IDs produce
two indistinguishable `log_id: 1` origin records. The fabricated field case does
not prove that the field itself is consumed: the command text already suffices.
These are synthetic matcher controls, not actual concurrent process controls.
No correction or successful rerun is claimed. Existing proxy behavior is unchanged.

## Capability inspection

One `id`/`capsh --print` inspection: collector uid 1000, no effective capabilities;
bounding set includes ptrace, BPF and perf capabilities. `sudo -n true` succeeded.
Kernel identifies as Linux 6.12.113 x86_64. `strace` and `perf` are installed;
`bpftrace` was not found. One `strace -f -e trace=execve,connect /bin/true`
probe successfully observed execve and exit 0. Root read tests found readable
`/sys/kernel/btf/vmlinux` and a tracing events directory. Reading
`/proc/sys/kernel/yama/ptrace_scope` failed because the path does not exist;
that absence is not proof that ptrace is unavailable. These checks do not prove
that a BPF program can load or that a tracer can observe the isolated account.
No tracing attachment to a session, BPF load, firewall edit or service launch
was performed. The one traced child and tracer exited normally. No process or
rule created by this investigation remains.

## Required integration boundary (proposal, not an implemented API)

Collector must start observation before releasing the existing isolated client,
retain trusted executable identity and process lifetime across fork/exec/exit,
and bind each proxy connection and request sequence to its actual sender.
RPC ID, duplicate arguments, timestamps and agent headers cannot be join keys.
A process identifier needs an incarnation identity; reparenting cannot erase
lineage. Connection reuse and transferred/shared sockets need explicit handling
or an ambiguous outcome. A socket owner snapshot alone is insufficient to prove
which process sent a request on a shared connection.

Keep raw evidence in the collector-private output tree. A proposed receipt must
include session identity, request sequence, connection identity, process
incarnation, verified executable identity, invocation lifetime, and evidence
status. Bind request completion by recorded request sequence. Missing, lost,
ambiguous, late and unanswered evidence must fail required attestation.
Close observation at session termination, clean descendants, drain and record
losses before reconciliation. Preserve both credentials, direct-backend denial,
source capture and publication scanning. Do not modify agent invocation syntax,
require wrappers/headers, or replace the frozen mo executable.

No adapter/interface is available to wire into the harness yet. Do not enable
collection from these controls. Remaining work includes selecting/proving a
supported observation mechanism, occurrence-safe native matching, removing
command-text attribution, and real frozen-binary service controls for ordinary
curl, dynamic urllib, native MCP and real mo push. Concurrency, late/reparented
processes, replay, failure, cleanup, credential/private-folder boundaries and
publication require actual controls; the synthetic cases above do not cover them.
No full CI, C build, real-service control or general test suite was run here.

## Safe matcher correction (subsequent implementation)

Removed Phase5 command-text upload attribution and inherited fetch-text exemption.
Native matching now retains request sequence, requires one response and a completed
native event, and consumes duplicate native calls by occurrence. No shell evidence
is accepted yet. Existing real push tests now require missing attestation without
collector evidence, even after successful execution, using /opt/mo-phase5/bin/mo.
The first focused rerun failed the old positive assertion (1 of 5); retained here
as evidence that the old test depended on the unsafe inference.

All six original negative controls now pass; their original four failures above
remain unchanged. An actual root strace launch of the frozen executable under
mo-phase5-session completed successfully. Private raw trace and stdout/stderr:
/tmp/mo-u2-probe-20261109 (directory 0700; files restricted). This probe proves
isolated-account tracing only, not request attribution or a failed mechanism.

No qualified adapter or trusted evidence API is delivered in this increment.
Do not wire command text back into match. Existing three-argument match API is
unchanged and fails closed for shell traffic. Remaining blocker is implementation
and validation of byte-stream sender/response ownership, PID incarnation and
exec identity across socket reuse/transfer and invocation termination. This is
incomplete work, not evidence that Linux tracing cannot support U2.
Real curl/urllib, concurrency, late-process and real-service traced push controls
remain unrun. No model runs, scored sessions, firewall edits, C builds or full CI.


## Further transport controls (2026-11-09; incomplete implementation)

Added `origin_observer.TraceRun(directory).run(command, timeout=..., env=...)`
and three lifecycle tests. Directory creation is exclusive, mode 0700; outputs
are private, the object is single use, timeout kills its process group and waits.
`capture_finished` means only that the tracer returned zero and an exit marker
was present. It is NOT a completeness or origin attestation. `qualified` is
always false. This helper does not handle descendants escaping the process group,
root tracing of an isolated uid, executable identity, or request reconciliation;
it is not suitable for harness integration. No harness/proxy change was made.

Red: test discovery failed importing the missing module (1 loader error).
Green: 3 lifecycle tests passed. These do not prove attribution.

Two actual timed transport probes used root `strace -f -yy -xx -s 1048576`
with `-u mo-phase5-session` against a temporary loopback HTTP server. Ordinary
curl and Python urllib both exited 0. Curl recorded 15 TCP annotations, one
sendto, one recvfrom, one execve and one exit; urllib recorded 5 TCP annotations,
one sendto, one recvfrom, one execve and one exit. Trace sizes were 186866 and
7322861 bytes respectively (ordinary file reads account for much of this).
Raw traces remain in `/tmp/mo-u2-transport-ednpkhhj`, directory 0700/files 0600;
only numeric summaries were printed. The server, both clients and tracers exited.
No firewall rules were added. No observed OS/mechanism blocker was found.

Existing proxy tests: 5 passed using explicit
`MO_EVAL_BINARY=/home/exedev/code/mo-m6/build/gcc/debug/mo`, including actual
service/push execution (not traced push attribution). Original six negative
origin controls passed. `git diff --check` passed. No models or scored sessions.

U2 remains incomplete: no stream-offset/request-sequence join, trusted executable
identity/incarnation ledger, shared-socket ambiguity detection, response receive
binding, replay/truncation rejection, traced real push, concurrent/reparented/late
process controls or integration contract has been completed. These are missing
implementation, not a demonstrated capability blocker. Do not enable collection.

### Bounded trace decoder component (2026-11-09)

Added `origin_trace.py` and synthetic `test_origin_trace.py`. Initial test run
failed with missing `origin_trace`; five tests now pass. Supports PID prefixes,
hex strings, unfinished/resumed scalar calls, successful partial byte counts,
separate TCP streams, Content-Length messages, process generations and rejection
of observed shared ownership, fork inheritance and repeated connection identity.
Diagnostics contain no source text. Receipts omit headers and body values.

This is an executable partial component, **not a qualification API**. Remaining
barriers include complete FD lifecycle tracking (especially aliases and sockets
inherited before the first observed transfer), syscall allowlist coverage, strict
request/response direction validation, and safe preservation of exact tool/argument
values. Current receipts provide structural body metadata, not exact arguments.
An image path is not a verified binary hash. Collector ownership, completeness,
executable hashes, proxy joins and real controls remain parent integration work.

Retained traces were read locally without printing buffers or credentials. The
transport files 0 and 1 each have one sendto and one recvfrom; both fail with
`unsupported TCP endpoint` (the supported grammar requires arrow endpoints).
File 0 has 45 reads and one write; file 1 has 215 reads and one write. Probe files
3180106 and 3180191 fail with `unsupported string encoding`; their read counts
are 148 and 960, write counts one each, sendto counts 14 and zero, recvfrom
counts 15 and zero. No complete request/response matches were returned for these
four files. Probe files required privileged read access; permissions were not
changed. These are precise retained-trace parsing barriers, not successful
transport qualification. No services or background processes were started.

### Actual trace compatibility (2026-11-09)

Scoped changes: origin_trace.py, test_origin_trace.py, this append only. No Git
operations, model runs, scored sessions, runtime edits, or new HTTP controls.
No background processes were started. Raw payloads and credentials were not
printed. Two sanitized actual-syntax tests first failed (2 errors, 5 passes);
after implementation all 11 tests pass with:
`python3 -m unittest discover -s learnability/phase5/evaluation -p test_origin_trace.py`.

The retained curl and urllib traces use TCP inode annotations even on their
transfers, not just before connect. Socket creation now establishes identity;
control calls do not pretend to transfer bytes. Standard escaped ASCII, quoted
quotes/backslashes, octal and hexadecimal C-string escapes are decoded. Relevant
truncated buffers fail. Dup aliases preserve identity; inherited descriptors
before first transfer are tracked and concurrent ownership fails closed. Shared
CLONE_FILES tables with tracked sockets are rejected. Process generation remains
in sender metadata. This is deliberately conservative, not full Linux FD semantics.

Both actual `/tmp/mo-u2-transport-ednpkhhj` files complete reconstruction:

| File | Requests | Responses | Request bytes/body | Response bytes/body |
| --- | --- | --- | --- | --- |
| 0 (curl) | 1 | 1 | 78 / 0 | 113 / 2 |
| 1 (urllib) | 1 | 1 | 119 / 0 | 113 / 2 |

Each message starts at stream offset 0 with ordinal 1. The zero request bodies
follow HTTP request framing (neither Transfer-Encoding nor Content-Length), not
an assumption that unframed responses are empty. Responses require explicit
Content-Length or HEAD/204/304 bodylessness. Direction is checked. Transfer-Encoding,
close-delimited bodies and informational-response sequencing remain unsupported.

Older probe traces 3180106 and 3180191 now parse to 0 requests / 0 responses each.
Observed evidence: 3180191 has 960 reads, one write, no sendto/recvfrom. In 3180106,
14 sendto annotations comprise 8 NETLINK, 3 UNIX-STREAM, 3 UNIX; its 15 recvfrom
annotations comprise 12 NETLINK and 3 UNIX-STREAM. Explicit non-TCP transfers are
excluded. Neither file supplies TCP HTTP bytes; zero receipts is not qualification
or proof that no requests occurred outside these files.

Receipt schema retains connection_ordinal, request_ordinal, sender (pid,
incarnation, exec_path, generation), byte_offset, byte_count, body_byte_count,
occurrences (offset/count/sender), JSON structure/tool-call/argument-count metadata.
It adds method for requests, status for responses, body_sha256, and for JSON bodies
canonical_json_sha256 (Python json sorted keys, compact separators, ensure_ascii).
These digests preserve exact raw body and normalized parsed JSON comparison without
publishing tool arguments. Treat digests as private correlation data, not public
receipts: low-entropy values can be guessed. This is not RFC 8785 canonicalization;
proxy integration must use the identical algorithm and independently compare bytes.

Remaining limits: scalar TCP I/O only, bounded HTTP framing above, conservative
fork/exec/shared ownership handling, no general accept/SCM_RIGHTS/CLONE_FILES or
CLOEXEC ledger, and no authenticated executable identity or proxy join. Unknown
inode transfers fail; arrow-annotated initial transfers remain supported for the
existing synthetic contract. Binary hashes, trace completeness and whole-U2
qualification remain separate, unfulfilled integration requirements.

### Partial connection join (2026-11-09; not ready for integration)

Added `origin_join.digests(bytes)` and `join(trace, rows)`. Join rejects absent
endpoint metadata, reused endpoint incarnations, duplicate sequences/receipts,
missing responses, status disagreement and body digest disagreement. Returns
observations with proxy sequence and trace sender; qualified remains false and
command_origin stays empty. This API trusts its private collector inputs; it
is not an authentication boundary or persistent replay ledger.

Phase5 subclasses the existing transport to record server-observed peer/local
endpoints, connection request ordinal and raw/canonical body digests. Response
bytes are captured from the upstream response read. Credentials still forward
and redact through existing code. Transport failures lack response digest evidence
and reject. Trace adds endpoint only when an arrow annotation supplies it;
inode-only captures remain unjoinable. No timing/RPC/command-text fallback.

Focused checks: 16 origin tests passed (11 existing trace, 3 observer, 2 join);
5 existing proxy tests passed against build/gcc/debug/mo, including untraced
service/push and credential forwarding/redaction. Join rejection test covers six
mutations. No initial red run was performed for this increment. No actual traced
curl/urllib/proxy or traced push positive control was completed. No executable
hash receipt API was completed. Thus this increment does NOT fulfill U2 or the
requested bounded component acceptance conditions; parent must not integrate it.
No C build, full CI, models, scored sessions, Git operations or firewall changes.
Test processes completed. No raw trace was printed or new trace retained.

Binary SHA256 at verification (not execution authentication):
7c3c05e122e41159eb27dd099bda276df408d998dfac96146d5c42743d20d182.
Remaining: collector-side exec-stop hash/argv receipt with generation binding,
endpoint evidence for inode-only traces, actual controls, malformed input bounds,
persistent session/replay protection, response framing extensions as required,
and independent review. Existing trace limitations continue to apply.

### Live development controls declared before execution

Three cases: ordinary curl tools/call (one request), dynamic Python urllib
 tools/call (two identical RPC IDs, two requests), real migrating mo push with
one unrelated concurrent curl (push request count determined by actual bounded
upload, exact coverage required). Collector starts ordinary commands via strace.
Private TCP table snapshots at proxy receipt bind inode to endpoint; no identity
is inferred from RPC ID. Failures and retries retained, never scored. Accepted
push report/ordinal remain separate from origin and qualified stays false.

Live results: attempt 1 `/tmp/mo-origin-live-vkzbxwwk` failed all three:
curl 1/1 trace messages, 1 proxy request, absent endpoint join; urllib one
request/response HTTP 400 and client exit 1; push plus curl 7/7 trace messages,
7 proxy requests, absent endpoint join. Both ordinary clients omitted the
required MCP-Protocol-Version header. Push itself exited 0. These failures
remain in the private artifacts, not replaced by retry results.

Minimal fix: parse_trace accepts keyword tcp_snapshots (collector-private
/proc/net/tcp strings). Established entries resolve trace TCP inode annotations;
multiple endpoints for one inode reject. No RPC/time/command-text fallback.
Controls snapshot at proxy request receipt while the client socket is alive.
Missing evidence still rejects at join. Ordinary requests now include the
service-required protocol header.

Attempt 2 `/tmp/mo-origin-live-n86usmpn`: all three passed with counts 1, 2, 7.
Attempt 3 `/tmp/mo-origin-live-mh5xkpie`: all three passed after adding explicit
HTTP 200 and exact six-push-plus-one-curl assertions. Each count equals trace
requests, received responses, proxy requests and joined observations. Urllib
reuses RPC id 1 for two actual requests. Push and unrelated curl are launched
concurrently with separate collector traces; sender paths are distinct.
Accepted ordinal is 1, stored separately with accepted-report.json. This does
not authenticate push identity. Eleven existing trace tests also passed.

API: test_origin_live.run_controls() returns three summaries and retains a new
0700 temporary root each invocation; direct CLI exits nonzero if any fail.
origin_trace.parse_trace(text, default_pid=None, *, tcp_snapshots=()) remains
unqualified. Each root holds per-case proxy.jsonl, tcp.json, service.log,
primary/{trace,stdout,stderr}, and push/unrelated equivalents. Raw evidence is
private and was not printed. All clients/tracers/services were waited and
stopped; no firewall changes. No Git, harness, grade, models, C build or full CI.

Remaining proof gaps: executable hash is checked before the test only, not
exec-stop authenticated; path checks are observations only. TCP snapshots are
trusted ephemeral collector inputs, not a persistent inode incarnation ledger.
No escaped-descendant, shared-socket, network-namespace, replay/session or full
capture-completeness qualification is supplied. HTTP support remains bounded.
Concurrent launch does not establish overlapping sends. qualified remains false
and command_origin empty. Parent owns independent review and integration.

U2 bounded identity component (2026-11-09): `origin_identity.Capture` reserves a
collector nonce before capture, pins a read-only executable and protected ancestor
ownership, seals exact raw trace/proxy-log hashes and file metadata in a private
atomic manifest, and consumes the session-bound nonce once. `consume` reparses the
bound raw trace; successful exec argv, process incarnation and exec generation
travel with sender observations. Raw argv is private and must not be published.
Identity classification is separate from completeness and never sets qualified.
The caller must enforce a distinct unprivileged session UID without mount or
credential-changing powers; same-UID sessions are rejected. Existing live controls
use the collector UID, so their joined observations do not establish this boundary.
No claim of six verified push observations is made for that setup. TCP snapshot
binding and isolated live integration remain outstanding, as do completeness,
shared-socket and escaped-descendant proof. A single exit mark is not completeness.
Red: identity test discovery failed with missing module (one loader error).
Green: 10 identity tests, 11 trace tests and 2 join tests passed.

### Isolated identity integration (2026-11-09)

Declared and ran three development cases per attempt: curl 1, urllib 2 with
reused ID, push plus unrelated curl 7. Root collector uses strace -u
mo-phase5-session with ordinary argv. No identity UID check was relaxed.
Current binary SHA256 7c3c05e122e41159eb27dd099bda276df408d998dfac96146d5c42743d20d182
was copied to a temporary root-owned 0555 executable under /opt (0755 parents).
Capture pins it before execution and verifies it at seal/consume. TCP tables
sampled at proxy receipt are protected and sealed with session, nonce and raw
trace digest; consume reparses them with the bound trace.

Three attempts retained: /root/mo-origin-live-qux40pad and
/root/mo-origin-live-xiqwg8ul passed curl/urllib but push exited 1 with E1102
because the compiled library path could not be traversed. Copying lib alone did
not change that compiled path. /root/mo-origin-live-pdx8dwz5 passed all three:
1 request/response/proxy/join, 2 request/response/proxy/join, and 7 of each.
Identity consumption classified 0/1, 0/2, and 6/1 push/nonpush respectively.
Actual accepted service ordinal was separately 1. Raw artifacts remain root
private (0700 directories, 0600 evidence); no credentials published.

sudo -n setfacl failed exit 1: `sudo: setfacl: command not found`.
Temporary other-execute on /home/exedev resolved library traversal; restored to
0750 after capture. CLI now restores the prior mode in finally. Temporary
installations removed; pgrep for isolated account returned 1 (no processes).
No firewall rules added, no Git, harness, grading, model, C build or full CI.
27 focused origin tests pass. Executable control command:
`sudo -n python3 learnability/phase5/evaluation/test_origin_live.py`.
Completeness, shared sockets and escaped descendants remain unqualified; all
results keep qualified=false and command_origin empty. This is bounded identity
integration, not full U2 qualification or backend-denial qualification.

### Bounded integrity controls (2026-11-09)

Six cases declared before launch, two actual runs retained at
/root/mo-origin-integrity-xyl_fdx9 and /root/mo-origin-integrity-ep9g67bg.
Both runs: normal 1 process complete; fork/dup before first send 2 processes
complete and decoder rejects ambiguity; actual two senders on inherited TCP
socket 2 processes complete and decoder rejects; setsid/double fork 3 processes
complete; escaped timeout 2 observed processes, only one terminal, completeness
false, then dedicated-UID cleanup; known backend port direct curl exits 7,
proxy curl exits 0. All six declared checks passed in each run. These socket
controls intentionally reject legitimate inherited usage rather than crediting
the wrong sender. The backend control uses a bounded local HTTP server and the
same owner OUTPUT REJECT rule as the protected harness; it is not a rerun of
credential-bearing Mo push. Prior isolated push evidence remains separate.

TraceRun now collects fcntl/isolation operations and returns a process ledger,
integrity_errors, process_completeness_verified and capture_finished. Ledger
requires launch exec, registered fork children and terminal status for every
observed PID; unfinished EOF, unknown records, reuse, ambiguous child ordering,
isolation transitions and strace diagnostics fail closed. PID reuse is an
unsupported case, not inferred continuity. Single exit marker fails. This is
bounded strace evidence, not independent proof that every possible kernel
escape is observable. qualification(facts) requires each named fact to be exactly
True; absent/unknown values fail. This internal conjunction is not an authenticated
attestation interface. No caller is wired to set qualified true.

Socket fallback receives a fresh serial, and reused endpoint fallback rejects;
existing close/reuse test initially failed (28 passed, 1 failed) and corrected
run passed all 29 tests. Logs retained in /tmp/mo-origin-integrity-unit-1.log
and -2.log; sanitized live summaries in /tmp/mo-origin-integrity-live-1.log
and -2.log. Raw evidence remains root-private, no raw credentials printed.
Existing identity tests exercise session/replay consumption; no identity API
was changed. General TCP, descriptor transfer and arbitrary process reuse remain
unsupported; scalar HTTP subset only.

Both actual runs restored exact `iptables -S OUTPUT` baseline and ended with
zero dedicated-UID processes. Cleanup uses the protected harness end_all pattern
(pgrep/pkill restricted to that UID, bounded retries), no foreign-user kills.
Servers, proxy and tracer were stopped/waited. TraceRun alone still does not
perform whole-UID cleanup: parent integration must invoke protected cleanup,
verify zero survivors and bind the resulting facts. Overall qualified=false.
Parent harness integration, protected regrader, native matching and model proof
remain separate. No Git, subagents, harness/grade/model edits, models/scored
sessions, C build or full CI. Only observer, trace, new integrity tests and this
append changed.

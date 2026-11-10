# U1 capability inspection — collection blocked

2026-11-09. Baseline 4dffd7e2776a3ff7a9a7245c7131b8ef330308d5,
existing fix/phase5-evaluation-capture checkout. This is development evidence,
not readiness, scored collection, or a claim that every possible channel is absent.
R4/R7 and KTD1 remain unsatisfied.

Before any client executable was invoked, this pass declared **zero model-serving
characterization sessions**. Actual counts: Codex 0, Claude 0; scored sessions 0.
Metadata-only invocations: Codex version, exec help, schema generation (3);
Claude version and help (2). All completed. No credentials were read or printed,
no service was launched, and no authentication or baseline artifacts were changed.

## Exact local capabilities

Inspected the frozen installation, not PATH aliases:

- `/opt/mo-phase5/bin/codex`: `codex-cli 0.166.0`, SHA-256
  `b8a11fedf24b2dfb59a70c9e52b030f9a508dcd49cd2dd0d41956f4c785156b8`.
- `/opt/mo-phase5/bin/claude`: `2.1.317 (Claude Code)`, SHA-256
  `eb224419e344575faa4a1f39b290b649425a10cd5ee2ef1d003480cd9ae57eb0`.

Generated schema and Claude help are retained in the mode-0700 existing harness
state subdirectory `/home/exedev/.mo-phase5-harness/u1-capabilities-20261109/`.
Command: `codex app-server generate-json-schema --out .../schema`.
An initial lookup for `schema/v2/Turn.json` failed: Turn is instead embedded in
`codex_app_server_protocol.schemas.json`, definitions.v2.Turn. That definition
was subsequently read successfully.

The installed Turn has id/items/status and lifecycle fields, but no actual-model
field. ThreadStartResponse has model/modelProvider/reasoningEffort; initialization
configuration is insufficient under R4. ModelReroutedNotification does exist:
required fromModel, toModel, reason, threadId, turnId; its reason enum contains
highRiskCyberActivity. It can retain a routing event, but absence of such an event
is not positive per-request serving-model evidence or proof of complete coverage.
No transport switch or model substitution has been made.

## Source evidence and original streams

Official sources fetched on this date:

- https://learn.chatgpt.com/docs/non-interactive-mode — machine-readable output
  describes lifecycle/item events and usage at completion. Its example does not
  establish an actual-model field. This agrees with original slots 1 and 3:
  thread.started has thread_id; turn.started has only type; turn.completed has
  usage. These streams do not establish the requested gpt-6.1-sol serving identity
  or xhigh execution. Requested flags cannot supply that evidence.
- https://learn.chatgpt.com/docs/app-server — documents version-specific schema
  generation and model/rerouted as notification of service routing to another
  model. Neither this notification nor the inspected Turn schema establishes
  every serving request's actual model. Current docs alone do not prove behavior
  of the pinned executable.
- https://code.claude.com/docs/en/headless — documents stream JSON as a supported
  CLI output channel. Original slots 2 and 4 have assistant envelopes containing
  session_id, request_id, and message.model (claude-opus-5-5), plus terminal
  modelUsage. These are candidate typed metadata paths, not a new characterization
  proving complete primary/auxiliary/fallback coverage. System init model alone
  is requested configuration and must not pass.

Original streams were inspected read-only by event type and field names, without
printing message bodies. No original score or artifact was rewritten.

## Blocker and integration instructions

The precise unresolved capability is a supported, trusted source binding **every
primary model-serving request** to its actual model and this collection session
for frozen Codex exec 0.166.0. Neither the original JSONL nor the inspected local
app-server lifecycle schema supplies it. This inspection does not establish that
all other supported telemetry channels are impossible. Claude complete-coverage
qualification also remains outstanding. Consequently U1 is **not ready**.

Do not integrate the existing recursive harness.models result as R4 proof. Keep
collection disabled until the parent establishes and characterizes a supported
channel for both pinned clients, with a separately declared bounded count and
protected raw capture. Do not infer serving identity from missing reroutes,
requested flags, assistant claims, or model keys inside tool results.

No adapter schema was invented, and no adapter or tests were implemented in this
blocked inspection. Red/green evidence is therefore **not available**, rather
than passed. The next implementation must write failing typed controls first:
valid session/request-bound metadata; requested-only, fake tool/assistant model
text, wrong session, missing/conflicting/truncated coverage; retained fallback
that forces a miss; and separately retained auxiliary events. Only then implement
extraction against the established schema and provide harness integration.
Parent owns authoritative verification, integration, independent review and PR.


## Additional exact-source investigation (2026-11-09; incomplete)

This follow-up made zero model-serving invocations (Codex 0, Claude 0), zero
scored launches, and ran no tests. It does **not** establish a supported-channel
limitation. A concrete candidate was found and still requires protected bounded
characterization; do not close U1 on the earlier schema inspection.

The installed executables again reported Codex 0.166.0 and Claude 2.1.317.
Read-only public source retrieval used the exact `rust-v0.166.0` tag in
`openai/codex`. Raw source, GitHub tree (including its resolved SHA), and a SHA-256
manifest are retained under the mode-0700 directory
`/home/exedev/.mo-phase5-harness/u1-telemetry-20261109/`. No credentials were read.
The first guessed `otel_manager.rs` path returned 404; the GitHub tag tree then
identified the actual files. A first unquoted tree URL failed in zsh before a
request; the quoted retry succeeded. These were source retrievals, not model probes.

Concrete findings, with paths relative to `codex-rs/` at that tag:

- `codex-api/src/sse/responses.rs:35–104`: HTTP response header
  `openai-model` becomes `ResponseEvent::ServerModel`; upstream request ID is
  also retained by the stream wrapper.
- The same file, `response_model` at lines 193–211 and header helper at 299:
  server metadata comes from `response.headers`, then top-level `headers`, with
  case-insensitive `openai-model` / `x-openai-model`. This is different from an
  arbitrary recursive model key. A source test named
  `process_sse_ignores_response_model_field_in_payload` explicitly distinguishes
  payload model from these header paths.
- Lines 622 and 640–650: trace logging records raw SSE data before parsing;
  parsed server model changes are emitted with duplicate suppression. This is a
  **candidate diagnostic capture route**, not yet a qualified complete record.
  HTTP headers outside SSE, WebSocket handling, retries, stream truncation,
  compaction/auxiliary requests, and session/request binding need tracing through
  the remaining consumers and live protected capture. Absence of a change event
  must never be treated as positive model proof.
- `otel/src/events/session_telemetry.rs:155–157`: `with_model` assigns supplied
  model metadata. Lines 1409–1410 classify ServerModel/ModelVerifications events
  by kind. These observations do not establish that ordinary OTel `model` is
  the provider-served identity; its caller chain and exported attributes remain
  to be checked. The fetched config schema is retained alongside the source.

Official pages actually opened:

- https://learn.chatgpt.com/docs/config-file/config-advanced — documents OTel
  export and model context, but current documentation cannot establish the
  pinned binary's response identity semantics.
- https://code.claude.com/docs/en/monitoring-usage — supported Claude monitoring
  route to qualify alongside stream JSON. Pinned local help also exposes
  `--debug [filter]` and `--debug-file`. Neither debug logs nor OTel complete
  fallback coverage were exercised or proven in this follow-up.

Parent integration: no new adapter is supplied and no harness changes were made.
Keep readiness false. Next work should follow the concrete ServerModel and raw
SSE trace candidates, establish collector-owned session/request completeness,
then declare a bounded run count **before** any model launch and use the existing
protected harness. Write typed negative controls before implementing extraction.
Do not turn this unfinished investigation into a claim that all supported routes
were exhausted. No client/model substitution decision is justified yet: a
candidate exists within the exact pinned source and remains unqualified.

## Bounded live qualification (2026-11-09)

Parent declared exactly one development Codex session and one development Claude
session before launch, no automatic replacement, zero scored sessions. This pass
executed that declaration. Each client: attempted **1**, completed **1**, failed
**0**, exit status 0. Both executed the innocuous printf command and reported its
output. Qualification failures are separate: Codex lacks actual-model evidence;
Claude passes narrow envelope accounting but complete serving coverage is unproven.
No session was repeated. Scored launches: **0**.

Private evidence: `/home/exedev/.mo-phase5-harness/u1-live-20261109/` (0700),
with `run.py`, both JSONL/stderr captures, `counts.json`, and
`sanitized-summary.json`. Files created by the runner use umask 077. The runner
used existing harness check/private-path checks, separate installed session UID,
credential-preserving reset before/after each client, and frozen version checks.
No fixture service or scored queue task ran. Provider credentials stayed in the
existing installation/harness inventory; raw credentials were not copied into the
repository or printed. Cleanup completed; pgrep found no session-user processes.
The first attempted note read used the nonexistent repository-relative
`evaluation/` path; the actual path is `learnability/phase5/evaluation/`.

Codex requested gpt-6.1-sol/xhigh on frozen 0.166.0. Capture enabled
`RUST_LOG=codex_api=trace,codex_core=debug`, with stdout and stderr opened by the
collector outside session-readable directories. The actual transport was WebSocket,
status 101. There were three timing events, zero raw `SSE event:` records and zero
`openai-model` header occurrences in stderr. Timing event model labels were
`gpt-6.1-sol`; these are **requested**, not proven actual models.

Additional exact-tag source retained privately in
`u1-telemetry-20261109/responses_websocket.rs`, fetched from
https://raw.githubusercontent.com/openai/codex/rust-v0.166.0/codex-rs/codex-api/src/endpoint/responses_websocket.rs :
lines 266–271 construct timing context from `ws_request.model`; lines 980–1010
emit that context with session/thread/turn IDs and timing payload. Therefore those
bindings do not convert its requested model into serving identity. Lines 302–303
emit handshake ServerModel internally; lines 910–925 extract event header models;
line 396 sets upstream_request_id to None. The existing SSE source lines 193–211
and 299–358 define response.headers before top-level headers, case-insensitive
openai-model/x-openai-model, including first-array-element values. Neither payload
`model` nor a recursive tool key supplies this evidence. The assumption that the
SSE trace would expose this run's actual-model headers failed. No forced-HTTP
rerun, transport substitution or additional session was attempted.

Claude 2.1.317 requested claude-opus-5-5 with stream-json, verbose and supported
`--debug`. It emitted two assistant envelopes, each with session_id, request_id
and message.model=claude-opus-5-5. One success result had matching session_id and
terminal modelUsage containing only claude-opus-5-5. The typed adapter's live
accounting passed. This observation does not prove that every retry, failed
request, compaction or auxiliary request emits an assistant envelope or appears
in modelUsage. Debug output did not establish an independently authenticated
complete request ledger. No fallback was observed in these typed envelopes;
absence is not proof that fallback coverage is complete.

New `model_adapter.py` exposes `codex_header(provider_event)` and
`claude_metadata(events, expected_session)`. The former only interprets exact-source
header paths; its input must already be a trusted provider event. It cannot bind a
header to a request itself. The latter retains request/model pairs and terminal
model names, rejects conflicting request identities, wrong sessions, missing or
truncated terminal accounting, and retains substituted/auxiliary model names while
failing their accounting. `complete` means narrow Claude envelope accounting only;
`qualified` always remains false. Neither helper is an authentication boundary.
Collector ownership prevents file modification; it does not authenticate arbitrary
text that a client or tool can emit into its output descriptors.

Tests were written first: discovery failed with ModuleNotFoundError before the
adapter existed (red). Seven focused tests then passed (green), including typed
positive sequences, fake nested keys, wrong sessions, missing/truncated records,
conflicting retry identity, fallback/auxiliary retention and terminal mismatch.
The header positive is synthetic exact-source characterization: **no live positive
Codex actual-header sequence was obtained**. Retry and auxiliary completeness
remain unsupported and cannot be justified away using that synthetic test.

Integration: import the helpers for diagnostic extraction only. Supply the expected
session from a protected launch/session record and retain originals privately.
Do not wire `complete` to the R4 gate or use requested timing labels as actual models.
A future collector needs authenticated request start/end/retry coverage, transport
headers and auxiliary/fallback accounting before qualified can become true. Current
U1 remains blocked; no claim that every supported channel is impossible follows.
No harness edits, C changes, full CI, Git operations or PR work were performed.
Parent owns integration and independent verification.

The metadata-only live summary was scanned using the existing publication scanner
against both inventories: harness `tokens` (service and upgrade credentials) and
`secrets` (provider credentials), including its encoded-form checks. Scan passed;
no raw traces are published.

## Transport capability follow-up (no model runs)

Pinned Codex features list reports responses_websockets and responses_websockets_v2 removed; neither is a supported disable switch. Cached exact-tag core_config.schema.json declares provider supports_websockets, and core_src_client.rs:1188 gates transport on it. Official exact-tag source was fetched from https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/model-provider-info/src/lib.rs (the earlier core/src/model_provider_info.rs path returned404 because the module moved). merge_configured_model_providers:782–844 preserves built-in OpenAI identity/inference/auth and takes only catalog routing from its override; a supports_websockets override there does not establish HTTP selection. The new source is retained privately at u1-telemetry-20261109/model-provider-info_src_lib.rs, SHA256146d0cb03cf0e41cd83052a27ab428ffd953b2ddc2cc2dadf575a4a376236a7b. A separately named provider is an unproven candidate, not an implemented or approved collection route. No custom provider configuration, transport substitution, new model launch or scored session occurred. The actual-model gate remains unqualified.

Further source-only candidate: exact-tag model-provider-info to_api_provider chooses the same default endpoint from authentication mode when base_url is absent; is_openai compares the provider display name to OpenAI. A distinct configured provider key with that display name, requires_openai_auth=true and supports_websockets=false may therefore retain the built-in authentication/endpoint while selecting HTTP. This is source-derived, untested, and must receive a separate development declaration and verified request/model accounting before use. No configuration was applied and no model session was launched.

## Declared HTTP development characterization

One additional Codex development attempt was declared before launch in private
`/home/exedev/.mo-phase5-harness/http-development-20261109-one/declaration.json`.
Codex attempts 1, completions 1, process failures 0, exit 0; Claude 0, scored 0,
replacements 0. Including the prior characterization: Codex 2/2 completed and
Claude 1/1 completed. Qualification remains false; successful process exit is
not successful model qualification. Original 0/4 and the 3/4 gate are unchanged.

Cached rust-v0.166.0 provider source lines 469–486, 697–698 and 782–844,
client source 1188–1195, and ModelProviderInfo in the cached config schema were
checked before launch. A distinct development_http provider retained display
name OpenAI, requires_openai_auth=true, supports_websockets=false, absent base_url,
and default responses wire API. The override applied only through invocation
arguments; no permanent or evaluation configuration changed. The frozen binary
SHA-256 was checked as b8a11fedf24b2dfb59a70c9e52b030f9a508dcd49cd2dd0d41956f4c785156b8.

The simple printf metadata prompt completed within the 180-second bound. Capture
contains 63 parseable SSE events, including response.created, response.completed
and response.metadata, but zero openai-model mentions and zero extracted actual
header models. Requested labels and tool text are not model proof. Raw private
stdout/stderr retain lifecycle and any retry evidence, but authenticated exhaustive
request/session/retry coverage is not established. No retry probe was launched.

Private SHA-256: codex.jsonl
`d861ff353ed101ed4afaeec035ed3707d02a9c5a13ba37b6e82bcb3da935a22a`; codex.stderr
`05083513c8c9c16b584386f5acfff2cd3e0a840d4e9421a8755a0c8ff1a42ae9`.
The private sanitized-summary.json includes declaration/count hashes and event
type accounting. Raw captures are not published. No old traces needed compression.

Eight focused model_adapter tests passed, including added exact header precedence,
array/type rejection and fake message/tool/requested-only negative controls. These
characterize the existing parser; no parser change or invented trusted envelope
was needed. Full collector tests, full suite and CI were skipped in this bounded
pass; parent owns authoritative verification. Protected reset completed, session
and test accounts have no remaining processes, OUTPUT policy is ACCEPT, and home
mode is 0750. The first cleanup lookup used a nonexistent service account name;
it was corrected to the actual test account. Credential-preserving reset used the
existing operator snapshot. Publication scanner checks against service and provider
credential inventories passed for the metadata summary and owned Python files;
the note is scanned separately after writing. No scored integration is enabled.

## Source/evidence investigation follow-up (2026-11-09; no invocations)

U1 remains incomplete, qualification false, original 0/4 unchanged. This pass ran
0 tests, 0 client/model probes, 0 scored runs. The parent's eight adapter tests and
diff check are prior results, not checks repeated here. Only this note was appended.
Graph coverage reports both the private source cache and mo-m6 outside the indexed
project; the findings below use direct source reads instead.

**HTTP headers are observed independently of the SSE trace.** Cached exact-tag
`u1-telemetry-20261109/codex-api_src_sse_responses.rs:39–99` reads HTTP
`stream_response.headers`, extracts `openai-model` and `x-request-id`, and emits an
internal `ResponseEvent::ServerModel` when the former exists. The trace at line 622
prints SSE data, not those HTTP headers. Consequently the retained run's zero
header labels does **not** prove zero actual HTTP model headers. Whether that
particular response supplied a header remains unknown from this capture.
Official source: https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/codex-api/src/sse/responses.rs .

The two retained `response.metadata` events at private
`http-development-20261109-one/codex.stderr:68` and `:81` have `response_id` and
`metadata.openai_chatgpt_moderation_metadata`: prompt moderation results/category
flags plus generation/tool-call/tool-response containers (one also has commentary).
Only key/type structure was inspected for reporting; no content values are copied.
Neither event has model headers. The source at lines 193–290 distinguishes model
header extraction from moderation, verification recommendations, guardian assessment
and safety-buffering metadata. A moderation result, recommendation or retry-model
instruction is not proof of which model actually served a request. There is no
new actual-model-bearing field in these two retained metadata records.

**Telemetry does not repair this gap in the inspected paths.** Cached
`core_src_client.rs:3139–3171` forwards attempt/status/error/duration to
`record_api_request`; successful response headers are not parameters. Cached
`otel_src_events_session_telemetry.rs:752–824` records those request details, not a
server model. Its `responses_type` match at line 1409 reduces `ServerModel(_)` to
`server_model`, discarding the value in that classification. Cached client lines
2671–2826 forward stream events and record completion using response ID, upstream
request ID, usage and output items. Merely enabling that request telemetry does
not establish serving identity. Earlier requested-model timing labels remain
requested labels. These findings are bounded to these functions; they do not prove
that all downstream consumers or supported exports discard ServerModel.
Official sources: https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/core/src/client.rs
and https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/otel/src/events/session_telemetry.rs .

**Concrete next step, still source-only:** resolve the downstream consumer of
`ResponseEvent::ServerModel` and the `InferenceTraceAttempt` implementation using
the retained exact-tag tree (10,768 entries, truncated=false), then inspect whether
a supported export retains the server value together with request/session/attempt
identity. Guessed official paths `core/src/codex.rs`, `core/src/codex/mod.rs` and
`core/src/inference_trace.rs` returned HTTP 404; those failed lookups are retained
here rather than treated as evidence that the capability does not exist. The tree
instead includes `core/src/session/session.rs` and `core/src/stream_events_utils.rs`.
No supported no-patch evidence route has yet been demonstrated. The precise current
collection blocker is missing exported, trusted actual-model identity correlated
to all serving attempts, including retries and auxiliaries. This is an evidence
blocker, not a finding that the provider lacks such a capability. Neither a further
model probe nor TLS interception follows from this investigation.

**Claude boundary:** retained `u1-live-20261109/claude.jsonl:6,9` contains two
assistant envelopes with request IDs, session IDs and the expected message model;
line 10 contains terminal accounting with exactly the expected model key. This
supports identity assertions for those observed envelopes and agreement of their
model names with terminal aggregation. A terminal per-model aggregate is not a
per-attempt ledger: it cannot enumerate failed-before-envelope requests, retry
attempts, compaction or auxiliary calls, or prove their absence. No invocation was
made. The local pinned `~/.local/share/claude/versions/2.1.317` is an ELF executable;
this pass did not establish a source-level exhaustive logging contract from it.
No broader completeness claim is warranted from these retained records.

## Downstream source inspection (no model attempts)

Exact-tag core/src/session/turn.rs:3173–3184 consumes ServerModel only through maybe_warn_on_server_model_mismatch, with a once-per-turn warning guard. This inspected consumer is not a per-attempt export. The rollout-trace README documents CODEX_ROLLOUT_TRACE_ROOT as a supported local diagnostic export, but warns writes are best-effort. Its inference.rs response payload contains response/upstream IDs, usage and output items, with no server-model field; InferenceStarted instead records the context model. Thus enabling this trace alone does not supply actual-model identity. These are bounded source findings, not proof that every route is exhausted. No further model probe is justified by this export alone.

Official sources: https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/core/src/session/turn.rs and https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/rollout-trace/src/inference.rs . Private source hashes:

- core_src_session_turn.rs: 64d490381013b43eecfe3be130b424da4da0635463bf1fb69ac497c04e9bfd93
- rollout-trace_src_inference.rs: 932c8ec83ec91aa495ef438eb135f62fe8315fed4060115435da84db198bf66b
- rollout-trace_README.md: 4f5740cdd54dd3118b8b7ff3a71178cf760b8385432bf1dc485d97eeda869390

Eight focused adapter tests passed again at parent integration; whitespace check passed. This inspection added zero model attempts and no qualification. Protected publication regrading remains independently implementable while actual-model collection is blocked.

## Shared HTTP diagnostic hypothesis and bounded declaration

Exact-tag http-client/src/client.rs:179–188 logs response headers with method,
URL and status when RequestLogging is enabled. HttpClientBuilder defaults to
Enabled. core/src/client.rs:253–270 builds the API transport through
create_client_for_route. RequestTelemetry alone has no successful header field.
This is a new candidate diagnostic channel, not qualification. Retrieved source
hashes are in private resume-source-manifest.json and
resume-http-source-manifest-corrected.json. The first guessed directory
codex-http-client yielded no tree matches; the actual crate directory is http-client.

Before launch, declare exactly ONE additional Codex development attempt, zero
Claude attempts, zero scored attempts, no replacements, 180-second deadline.
Use the existing HTTP development configuration and frozen executable/model,
adding codex_http_client=debug to RUST_LOG. Hypothesis: the private diagnostic
captures provider response headers with actual-model and request-ID metadata.
Even a positive observation does not establish complete retry/auxiliary coverage.
Private output: ~/.mo-phase5-harness/http-header-development-20261109-one.
Preflight verified no retained work/hold and no session-user processes; frozen
installation check passed. No TLS interception or provider endpoint change.

The declared attempt completed once, exit 0; no replacement. Cumulative live
model attempts are now Codex 3, Claude 1, scored 0. The diagnostic captured seven
completed HTTP responses, including two /responses responses and 64 SSE events.
Both /responses header maps were present, without openai-model or x-openai-model.
They contained x-oai-request-id (not x-request-id). The initial summary checked
only x-request-id; subsequent key-only inspection corrected that distinction.
Safety-buffering model instructions and models-etag are not serving-model proof.
No actual model was established. This is evidence about this attempt, not proof
that every supported route is exhausted. Private raw logs and retained work are
in the declared directory. Session cleanup verified no surviving account process,
work folder or retention hold. No further probe is declared or justified here.

# Authenticated live-view implementation evidence

These are synthetic development checks, not scored diagnostic evaluation observations. Pilot
records, calibration classifications and evaluation gates are unchanged. No private campaign
materials were used.

## Native clients

`client_probe.py` reuses the preflight client's three-call prompt and approval modes. Its proxy
only forwards requests and records synthetic responses: discovery and both author/operator
calls are answered by the actual `mo serve`. Run it with an output directory outside the repo;
raw client streams can contain local hook output and are not publication artifacts.

```sh
python3 learnability/phase4/live-views/client_probe.py build/gcc/debug/mo /tmp/mo-live-views-final codex codex-approve claude
```

The recorded run used Codex CLI 0.166.0 with `gpt-6.1-sol`, `xhigh`, and Claude Code 2.1.318
with `claude-opus-5-5`. Claude is one patch newer than the original 2.1.317 preflight; this is
explicit evidence for the version actually run, not a claim to have rerun that older binary.
All client processes exited zero. [summary.json](results/summary.json) records commands and
model-visible names; [traffic.jsonl](results/traffic.jsonl) is the independent ordered wire
record, without credentials or raw hook output.

- Codex normalized `mo-state` and author `mo_state` to distinct hashed names. Its never-approval
  run invoked the two annotated operator tools and refused the unannotated author tool locally;
  the refused author attempt did not reach the server. Its explicit approve mode invoked all
  three in the requested order.
- Claude retained `mo-state` beside `mo_state`, discovered the tools through ToolSearch, and
  invoked all three in order under the explicit allowed-tools setting.
- Every forwarded call returned HTTP 200 and `isError: false`. The operator state was `"0"`;
  the author reply was `"author tool mo_state, count 0"`. No proxy synthesized either result.
- Codex's visible names are model-reported, as in the preflight. Claude's come from its init
  event. The wire routing evidence does not depend on either client's spelling report.

The executable digest in this traffic is
`c6307dbd63cf8f4cf88f3f95ac9e0d3267cd80380d86677a15dd47a823a21ade`. The subsequent memory-view
addition distinguishes the configured stack limit from allocated stack capacity; it does not
change discovery or routing. Its focused protocol/embedded-runtime checks passed, and the
later driver/sizing binary is identified separately below.

## Local behavior and bounds

The first added protocol test failed the three expected checks for the absent overview recovery
sequence and state/complete fields. After implementation, focused GCC runs of
`test_protocol_server`, `test_service_store`, and `test_lower` passed.

The protocol tests cover private nominal state with a scalar author tool, name collision,
257 timers and a full mailbox; view calls do not increment author admission. Timer truncation
counts include rows omitted before rendering as well as rows omitted by the JSON budget.
Existing rate tests now include refused operator calls. Supervisor tests pin newest-first order,
`after` filtering and the exclusion of handler stop text. The embedded-runtime test checks
annotations, validation and a manually specified saved Int encoding, while the ABI mutation
check rejects reordered observation fields. Store tests pin the post-snapshot log tail.

```sh
python3 ci/check_operator.py build/gcc/debug/mo
```

The real HTTP driver passed for saved scalar/container forms, field/index/map-entry paths,
private state, explicit truncation, token refusals, unavailable-tool refusals and unchanged
recovery/supervisor positions. It also runs a 16,000,000-byte Text case, checking both a truncated
whole-state response and an exact narrow path. [sizing.py](sizing.py) writes
[sizing.json](results/sizing.json), one such process measurement. Regenerated after the review
fixes, it records 16,000,110 encoded bytes, a 636-byte explicitly incomplete reply, and
114,364 KiB process high-water RSS. This includes compiler/runtime allocations and is a
measurement of that fixture, not a universal memory bound or an atomic cross-call observation.
The sizing executable SHA-256 is recorded in that file.

The registered `fuzz_mcp_views` target drives the embedded saved-schema renderer with bounded
arbitrary bytes under Clang ASan/UBSan. Two ten-second smoke runs completed 15,428 and 11,488
executions respectively without sanitizer or harness failures. The seed corpus includes a
hand-built valid Int value and truncated framing. Inputs and work are bounded; this empirical
smoke is not a proof. No new native untrusted-data reader was added: log metadata is copied
from the existing validated recovery/commit path, and JSON/schema shaping remains Mo code.

## Integrated verification and review

The final local executable SHA-256 is
`f222010857cbd984b0410a0c8d57f0a86d91bfd3ea8537a88f578b32eadbf850`.
The refreshed [client summary](results/reviewed-summary.json) and
[wire traffic](results/reviewed-traffic.jsonl) exercise that executable after simplification and
review fixes. All three client modes exited zero and routed the expected calls; Codex's
never-approval mode again refused the author call locally. Earlier evidence remains preserved.

- Full GCC, Clang and ASan/UBSan unit suites passed. All functional drivers ran with
  `MO_ALL_DRIVERS=1`, including the sanitizer executable, and passed. The combined run stopped
  at the final timing check: job-queue measured 0.1020 s against 0.100 s. An unchanged isolated
  rerun measured 0.0982 s. This failed combined result is retained, not relabeled a pass.
- After review fixes, `bash ci/check.sh gcc static` ended `check.sh finished: pass`.
  Focused Clang and sanitizer protocol, lowering/ABI and service-store tests passed.
  The real HTTP operator driver passed on GCC, Clang and sanitizer executables, including the
  new oversized-map-key regression. The final isolated timing check passed all 12 samples;
  job-queue measured 0.0955 s. Timing margins remain small and machine-dependent.
- Final renderer fuzz smoke completed 15,785 executions in 11 seconds under Clang sanitizers.
  The full static group passed, including grammar, dependency rules, fuzz seeds, formatting
  and clang-tidy. No formal semantics or critical native reader changed; no new Lean/CBMC
  proof claim is made.
- The simplification pass removed redundant record JSON wrapping, cached catalog names and
  removed two redundant comments, preserving proof tests.
- The [independent review](results/review.json) completed ten local review perspectives and
  fresh validation. Cross-provider review was unavailable because its lifecycle conflicted
  with in-turn collection; local adversarial review ran. All three retained findings were
  fixed: truncated map keys retain pair shape and omission counts; catalog tests independently
  pin exact acceptance, one-byte refusal and maximal pages; log tests assert recovery,
  failed-append preservation, snapshot-failure retention and recovery-snapshot clearing.
  The HTTP map regression first reproduced the reviewed failure, then passed after the fix.
  Additional Float edge-value coverage suggested by review remains a coverage limit, not an
  observed rendering failure.

The initial full GCC run caught two stale catalog-count expectations, which were corrected.
The new log tests initially confused request ID 123 with arrival time from the zero-valued
fixture clock; correcting that expectation made the strengthened tests pass. Local logs remain
in `build/u7-*.txt` on the machine that ran them, including failed runs; they were never
committed. No check was left running, and at that point no GitHub publication, push or merge
had been performed.

## Corrections after code review

A review of the published change found that this record and the renderer had defects. The
fixes are in the same pull request; this section says what changed in the evidence.

- The first `client_probe.py` inserted Claude's `--model claude-opus-5-5` before the prompt, so
  preflight's masking replaced the model name instead of the prompt in the recorded command.
  The run itself received both flags. The script now inserts them after the prompt, and the
  Claude command in [summary.json](results/summary.json) and
  [reviewed-summary.json](results/reviewed-summary.json) now shows the arguments that ran, in the
  order the fixed script passes them. No client was rerun.
- [sizing.json](results/sizing.json) is now produced by [sizing.py](sizing.py) from a committed
  fixture, and was regenerated with the fixed renderer. Its numbers replace the earlier ones,
  whose generator was not committed.
- The renderer changes (path skipping, budget accounting, truncation shape, paging and choice
  shape) are described in `docs/design/introspection.md`, "Authenticated live views". The
  operator driver and `fuzz_mcp_views` seeds were extended for them. The traffic files above
  describe the earlier executables and are kept as they were recorded.

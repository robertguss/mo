# Agent mistakes

The ledger the `correct` skill keeps: each kind of mistake agents have repeated in this repo, the
evidence, and what now stops it. A class is listed once it has happened twice. Levels, strongest
first: structure, compiler, CI check, test, prose. A class at `prose only` that recurs moves up a
level in the PR that fixes the recurrence.

Last run: 2026-10-18, PRs #80 to #121 and the project threads. GitHub held no review comments for
this range, so the evidence is fix-up commits inside PRs, the Review sections of PR bodies, red
`check` runs, and corrections in the project threads. Class 1 bumped 2026-10-24 for #178 and
#188's gap-review follow-up. Classes 6 and 7 bumped 2026-10-24 for MO-168's fresh-context review;
class 6 also bumped for MO-152's nightly workflow review and on 2026-10-25 for MO-153's
queue-preservation mutants and MO-154's repeated-digit duration-formatting mutant. Classes 6
and 7 bumped again on 2026-10-25 for MO-155's timer metadata and invalid service fix adapters.
Class 1 bumped on 2026-10-25 for Robert's PR #231 review of the service reference grammar.
Classes 6 and 8 bumped on 2026-10-26 for MO-156's independent descriptor review.
Class 1 bumped again on 2026-10-26 for PR #232's generated service names.
Classes 6 and 7 bumped on 2026-10-26 for MO-157's independent runtime review.
Class 7 bumped on 2026-10-27 for MO-159's admission, helper-context and recovery-attribution reviews.
Class 6 bumped on 2026-10-28 for MO-160's confidential scratch lifetime and Simulation store
refusal reviews; its additional evidence and enforcement are below.
Class 8 bumped on 2026-10-28 for MO-160's independent service-shape work-bound review.
Class 9 bumped on 2026-10-28 for MO-160's numeric fuzz corpus directory mismatch.
Class 6 bumped on 2026-10-29 for MO-160's composite fuzz acceptance oracle.
Class 12 added on 2026-10-29 for D324's correction to D322's notification regression list.
Focused update: 2026-10-30, PR #261's whole-chain review; class 6 bumped for VM wiping,
ABI refusal and the HTTP request-line boundary. The swapped CBMC bounds are a one-off below.
Class 6 bumped on 2026-10-30 for PR #263's missed three-level return case.

| # | Class | Evidence | Enforced by | State |
| --- | --- | --- | --- | --- |
| 1 | Docs that describe a changed behaviour left stale: `sample-blockers.md`, `architecture.md`, `bytecode.md`, `agent-guide.md`, comments, usage strings | 21 times: #81, #85, #94, #95, #101, #105, #107, #110, #111, #113 (2), #115 (3), #116, #119, #139 (`AGENTS.md`), #178 (`examples/README.md`, tools hold-back, CBMC inventories, `check.h`), #188 (unsupported gap descriptions after main advanced), #231 (Appendix B omitted service from item and had no service production), #232 (generated intervals started at 0 despite bytecode §3.5's 1-based convention) | `guide_claims` in `ci/check_cli.py` for the agent guide (#115); `ci/check_conformance.py` rejects unmapped reference blocks and unreviewed reference changes (#188), not stale implementation gaps or missing grammar productions; `tests/test_services.c` pins the approved start name and source-ordered interval names (#232); `review-change/rules.md` for the rest | check-enforced for guide claims and reference mappings; test-enforced for service names; prose only for implementation gaps, grammar-summary completeness and other docs |
| 2 | Library functions built as native signatures with hand-built Core where `docs/design/library.md` says they are written in Mo in `lib/` (`List.map`, `Map.update`, `List.sort_by_desc`, `List.count`, `Option.or_fail`) | #86, #87, #88, #97; moved into `lib/` by MO-46 part 2 (#121) | nothing: `make library-map` covers only `prims.def` and `hostops.def` | open: nothing stops a new one |
| 3 | Lean-dependent drivers skipped locally (no elan in cloud sessions), so hosted CI found the failures, or the PR merged without them | #97 (merged without Lean), #116 (2 red runs), #121 (2 red runs, body said drivers passed); nearly every PR body since #100 says Lean was skipped | AGENTS.md "a skip is not a pass" | prose only (hosted CI removed 22 Oct); cloud sessions don't run `.agents/setup` |
| 4 | PRs green alone merged onto a main that had moved, turning main red | #118 + #119 + #120: `tests/errors` pins E1003 at `main.mo:4097`, `lib/list.mo` takes one source slot, main red from f4e7f03 until #121; conflicts on the lowering table, `ci/check_cli.py` and `sample-blockers.md` in every sample PR after #106 | nothing (hosted CI removed 22 Oct) | open |
| 5 | The fresh-context review step skipped | #116, #117, #118, #121 (and #97 skipped a gate) | PR template Review section; `ship-task` | prose only |
| 6 | Tests or harnesses that pass without testing anything: loose exit-code matching, a count of 0 allowed, no negative control, surviving mutants, an empty fixture, a stale assertion, a CBMC specification that checks what a refusal leaves but not what an acceptance requires | 39 times across 18 tasks: #82, #87, #110, #113, #114, #115, #116, #138 (3 mutants survived), #139 (3 mutants survived), MO-168 (unterminated output and output-without-main mutants survived), MO-152 (scratch workflow check missed a mismatched artifact path), MO-153 (acknowledge and fail dropping other jobs survived), MO-154 (classifying repeated 1 digits as value one survived), MO-155 (invalid timer interval roots survived), MO-156 (duplicate service modules, empty tools and non-Time parameters survived; CLI probes used the wrong reader and an unused import; the CBMC bound could not reach tool decoding; taking docs and names from the first overload instead of the selected one survived), MO-157 (five mutants survived: extra work instruction, extra heap allowance, leaked reuse token, retained restart ticks and missing defect frames), MO-160 (scratch lifetime, Simulation memory-status mapping, blanket composite rejection, VM wiping, ABI refusal and a masked HTTP line boundary survived; Round E found missing listener restart, transient accept and pre-auth resource-failure coverage), MO-173 (syntactic jump references missed jumps inside unreachable join bodies; round 2 missed terminal operands and surviving scrutinee/jump-argument mutants) | `review-change` mutant step; `test-change` "see it red"; `ci/test_reference_examples.py` in `drivers` rejects both MO-168 mutants; nightly comparison and artifact upload share the job's `MO_CORE_DIFF_OUT`, removing the duplicated path; `examples/job-queue.mo` compares whole two-job queues after acknowledge and fail; `tests/test_format.c` checks repeated 1 digits; `tests/test_program.c` checks distinct timer intervals, valid roots and Duration types; `fuzz/fuzz_program.c` walks service bodies; `tests/test_services.c` rejects the three malformed descriptors, tests the combined handler limit and distinguishes two same-arity overloads' metadata after source destruction; CLI tests load binary emission and round-trip indexed text; CBMC reach checks cover decoder bodies; `tests/test_service_runtime.c` pins exact work/memory boundaries, live-token cancellation, queued ticks at restart and callback defect frames; `tests/golden/nested-choice-return/main.mo` pins two- and three-level returns and partial-return continuations; `tests/test_codegen.c` pins terminal operands and joins in scrutinees/jump arguments; `tests/test_listener.c`, `tests/test_listener_faults.c` and `tests/test_protocol_server_faults.c` cover Round E restart, retry and pre-auth refusal; MO-160 lifetime/status tests below, `fuzz/fuzz_mcp_value.c`'s independent acceptance oracle, `tests/test_lower.c`'s ABI refusal and `tests/runtime/http-check.mo`'s no-field boundary pair | test-enforced for MO-168 output boundaries, queue preservation, duration literal values, timer metadata, descriptor guards, selected-overload metadata, the five MO-157 regressions and the six MO-160 gaps, Round E listener restart/accept/resource-failure regressions and MO-173 nested continuations; structure-enforced for the nightly output path; prose only for other harnesses |
| 7 | Logic bugs in new checker, lowering, tools or runtime code that only a fresh review caught | 15 times: #81 (2), #85, #86, #113, #114 (2), MO-168 (declaration whitespace and declarations inside text), MO-155 (invalid adapters for `?` handlers and maximum-depth state types), MO-157 (recurring ticks starved the 65th startup timer in a 64-slot mailbox), MO-159 (pre-enqueue MEMORY treated as executed failure; descriptor-free control helpers refused; recovery/store failures blamed on the start handler) | `review-change`; `ci/test_reference_examples.py` checks declaration boundaries; `tests/test_fix.c` checks adapter eligibility and type depth; `vm/service.c` admits oldest timers with source-order ties and `tests/test_service_runtime.c` tests mailbox overflow; `tests/test_service_store.c` distinguishes admission from executed failure and pins recovery/store attribution; `test_core_check`, `test_bytecode_verify` and `ci/check_simulation.py` exercise descriptor-free helpers | structure- and test-enforced for timer admission; test-enforced for declaration detection, adapters and MO-159 admission/helper/attribution boundaries; prose only elsewhere |
| 8 | Input validation repeats unbounded work, omits work charges, or uses a size without a named limit | #81 (2), #86 (`src/check/bodies.c` missing `work(p)`), MO-156 (4096 descriptors rescanned one shared 1 MiB token name), MO-160 (4096 services rescanned 65535 shared tuple edges for shape reachability; Round E body parsing repeated per-read overhead for one- and two-byte reads) | `src/bytecode/validate.c` validates each canonical token string once; `src/bytecode/service-reach.c` shares recursive-component analysis between compiler and validator and propagates 64 service memberships together; `tests/test_services.c` bounds CPU time for shared-token and shared-shape graphs and checks recursive membership across batch boundaries; AGENTS.md "Every input has a named limit" and `review-change/rules.md` for other inputs | structure- and test-enforced for shared token and shape validation and Round E body-read coalescing (`protocol-request.c`, `test_protocol_server.c` and archived D329 runner); prose only elsewhere |
| 9 | Static drift: clang-format, a module-boundary include, an unchecked result, a fuzz corpus outside its target's directory | #81, #85, #111, #121, MO-160 (numeric seeds used `mcp_number` instead of `fuzz_mcp_number`) | `static` group: `make format-check`, `make deps`, `make tidy`, `ci/check_fuzz_seeds.py`; `fuzz-smoke` also runs seed admission before copying each target's corpus | nightly on main; format-check needs clang-format 14.0.6 |
| 10 | Boilerplate or wrong verification evidence pasted into PR bodies (Lean 4.29.0, Z3, Valgrind, a build path that doesn't exist) | #84, #89, #90, #91, #92, #95, #96, #98 | nothing (the required hosted `check` was removed 22 Oct) | prose only |
| 11 | Python caches committed | #102, #104, #105, #106 | `.gitignore` (#112) | enforced |
| 12 | Decision relays or regression lists contradict the approved rule | MO-160: a relay omitted D322's conditional method qualifier; D322's regression list requested HTTP 400 for array-params notifications despite item 10 requiring 202, corrected by Robert in D324 | `tests/runtime/mcp-rpc-check.mo` requires `Notification` for object and array params before method validation; `docs/design/services.md` records D324's 202 mapping | test-enforced for pure classification; HTTP mapping awaits the listener; prose only for decision-summary consistency |

On 22 Oct Robert removed hosted CI (no required `check`), so rows 3, 4 and 10 now
depend on agents running `ci/check.sh` groups locally.

MO-152 restores nightly checks of main, including Lean and static checks; it does not restore a
PR gate. Its review also corrected a one-off claim that GitHub concurrency queues every request:
the active run is preserved, but only the newest pending run is retained.

One-offs this run: macOS build breaks (#81; tests that create non-UTF-8 file names, which APFS
refuses, found by a local push on 22 Oct), a skill written into `.claude/skills/` instead of
`.agents/skills/` (#108), a Python line over 100 columns (#115).

MO-165's independent review caught a driver run against a stale sanitizer executable while a
separate build replaced it. That run is not passing evidence. Finish executable builds before
running their drivers, and check the gate's own exit status rather than a following `tail`.

MO-167's independent review caught control results whose source and commands existed only in
temporary files. The Fil-C trial report now includes runnable source and commands for the failing
cell round trip, its passing by-address control, and both bounds probes. These controls remain
replayable after the trial orb disappears.

MO-155's follow-up review found that extending the program fuzzer to service initializers exposed
a stale zero-capture assertion. The service corpus now includes a capturing initializer; the
fuzzer validates each captured binding instead. That seed failed the old assertion and passes
with capture validation. The C types cannot express interval-root validity or capture membership,
so the public checked-graph tests and fuzz invariants enforce those contracts.

Robert's PR #231 review caught the missing service production in Appendix B after section 11.5
had been added. The summary now includes the production and points to section 11.5 for clause
order and counts. No automated completeness check is claimed here; stronger enforcement remains
open. The same review approved the `?`-handler no-fix exception and refined D310 in D313 to
refuse `Result(State, E)` without fixes. That result-rule change is a new decision, not evidence
that the earlier implementation violated D310. Smaller fix edits, formatted adapters with names
derived from the tool, and retaining E0101 alongside resource-refusal remain design follow-ups
in `docs/design/services.md`, not implemented safeguards.

Robert's PR #232 review required `Module.%service.start` without `.0` and intervals numbered
from 1 in `every` clause order. The public source-to-image test failed on all three old names
before lowering changed, then passed. These are serialized strings, so C's type system cannot
enforce their spelling; testing the compiler's output pins the contract without making the
documentation a code generator. The same review required retaining tool descriptions and schema
parameter names now for MO-160. Their owned metadata, byte/text round trips and loader refusal
tests cover the added fields; this is a clarified requirement, not a claimed runtime safeguard.

MO-157's review found that scanning timers from index zero after every message could starve a
startup tick beyond mailbox capacity. Admission now selects the oldest due tick, breaking ties
in source order. The bounded mailbox limits admissions per poll; the regression runs 65 timers
with 64 slots and advances the clock during processing. Exact instruction and memory boundaries,
live reuse-token cancellation, restart with queued ticks and rendered defect frames are pinned
through public APIs. Types cannot enforce these temporal and resource contracts; the failing
mutants require execution tests, rather than another review instruction.

MO-159's simulation review found that the same MEMORY status crossed two different
boundaries. A direct send refusal is an admission bug; a handler reply is a supervised
failure. Simulation now classifies them at those separate call sites without changing
production persistence. Aggregate-limit, exact-limit and executed-memory tests pin the
distinction. Controls select no service, so Core and bytecode check their fixed signatures
without requiring descriptors. Only start/send validate descriptor indices. Source,
Core-text and hand-made-image tests cover standalone and imported helpers. C types cannot
express the temporal status origin or the absence of a descriptor requirement.

PR #252's wording review found that recovery and snapshot failures inherited the start
handler's identity. The fault-injected restart first reported `Main.%service.start`; it now
reports `Simulation.restart`. Handler identity is cleared outside invocation and value
acceptance, and replay supplies its own identity. Store-load, invalid-data and initial-snapshot
tests pin the operation names. C's types cannot distinguish these phases of one startup call.

Class 7 recurred during that correction: a new early Simulation member check rejected user
helpers, changed the required headline and left the call-result flag unset. Removing the
early branch and specializing the existing unresolved-member diagnostic preserves dispatch
and its initialized output. `test_program:simulation_inputs` checks helper acceptance and
parenthesized refusal; the error golden pins the original headline and revised reason. Both
new assertions failed before the fix. No new dispatch path is needed for diagnostic wording.

Class 6 recurred in MO-160: the confidential scratch test checked live bytes after growth and
whole chunks at destruction, but missed retained old bytes between those events. Removing only
the move-time wipe passed that test. `tests/test_protocol_memory.c` now checks old storage before
destruction and truncated tails after shrinking both the newest and an older block. Each of the
three isolated wipe-removal mutants fails its corresponding assertion; retained prefixes and a
neighboring allocation stay intact. Shrinking each block to zero also checks its discarded bytes;
a mutant that skips only zero-length wipes fails both checks. This raises class 6's total to 29
occurrences across 17 tasks. C types and final-release checks cannot enforce when discarded bytes
become zero, so the test observes each lifetime boundary through the scratch API. No production
defect was found.

Class 6 recurred in MO-160's store-status split: the host test pinned the new memory status,
but removing its Simulation mapping still passed. The existing compiled Simulation fixture now
runs under store allocation refusal and full handle exhaustion, then succeeds after both are
released. The missing-memory-mapping mutant fails one assertion. This raises class 6's total to
30 occurrences across 17 tasks. The host's enum cannot enforce the caller's distinct, unchanged
message, so the regression executes that boundary. No production defect was found.

MO-160's composite fuzzer accepted INVALID after every successful Mo decode. A bridge that
refused every value survived five valid controls, although the kept unit suite caught it.
The fuzzer now requires acceptance unless an independent pairwise oracle finds duplicate
Int keys or nested Set members. It compares nested sets without depending on their order or
the native table builder. Types cannot distinguish legitimate duplicate refusal from blanket
rejection, so the executable oracle checks both. This is the 31st occurrence across 17 tasks;
no production defect was found.

MO-160's owner questioned a JSON-RPC branch after reading a relay that omitted D322's
"If a method member is present" qualifier. Robert's original approval and its complete Notion
record already made the branch conditional. The parser was unchanged; the RPC fixture now
also covers an unsolicited response with malformed id and params. The correction concerns
interpretation of a shortened relay, not a new public decision.

D324 corrected the contradictory HTTP 400 line in D322's regression list, not item 10's
notification rule. The pure parser was already correct. Its fixture now explicitly requires
array params to produce `Notification` for every method, including `tools/call`, while
requests with array params still fail method validation. The listener is not in this unit,
so these tests do not claim to verify HTTP 202 or an empty wire response.

PR #261's whole-chain review found three further class 6 gaps. Arena observation missed VM
operand and process wipes; successful embedded construction missed ABI refusal; an overlong
Authorization field masked the request-line boundary. The existing tests now observe native
operand erasure and process bytes before release, reject otherwise valid swapped HeaderProblem
tags, and accept/refuse a no-field request at line limits 18/17. All four compiling mutants
that survived review now fail, with unchanged-production controls passing. This raises class 6
to 34 occurrences across 17 tasks. C types cannot enforce erased storage, loaded tag order or
runtime byte limits; tests exercise those contracts without adding runtime hooks.

The same review found swapped service-reader CBMC unwind bounds: loop .2 is the inner
four-byte case loop, and .3 is the outer twelve-byte shape loop. The original proof failed its
line-474 unwinding assertion. Correcting only those bounds passes all 11,274 properties with
unwinding assertions retained. This is proof configuration, not a production-reader fix.

MO-160 part 2 repeated class 7 during diagnostic integration. E0106 ran before E0101,
and E0105 inspected private service state instead of only wire types. Robert required
preserving E0101 precedence and excluding private state. The service checker now runs
wire checks after existing state-result validation across all services and projects the
wire signature before traversing visibility. The unchanged E0101 predicate golden and
the E0105 update fixtures distinguish both mistakes, including imported private state,
recursive generic public arguments, hidden update replies and application errors.

PR #263's review found that syntactic jump references included jumps inside skipped join
bodies. The original two-level fixture passed while a three-level case still aborted.
Codegen now walks reachable paths before emission, visiting a join's rest before its body.
The golden regression fails on the prior fix and checks all three outcomes plus a partial-return
continuation. This is class 6's 35th occurrence across 18 tasks. C types cannot distinguish
reachable jumps from syntactic references; the traversal and executable regression enforce it.

Round E, PR #262 (2026-10-30): listener restart and transient-accept failures lacked
regressions, pre-auth resource failures silently closed, and body work depended on
read fragmentation. Fixed at the test level: fixed-port reopen after server-first
close, per-errno accept injection through a real protocol server, pre-auth invoke
and allocation injection, and 16 KiB bodies at one- and two-byte read quanta.
The archived D329 runner now measures both plain and escaped small-read bodies.
Dead PS_ABANDONED branches were removed structurally; pending slots still stay
occupied through the service reply. These are further class 6/8 evidence; no
supervisor-event behavior was changed.

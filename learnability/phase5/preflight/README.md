# Upgrade feasibility preflight

Predictions recorded before running `probe.py`:

- Extracting the task queue's three concrete state declarations into `UpgradeOld`
  and exposing their constructors permits a pure migration to a new state with
  one added field, without changing the source language.
- Calling that migration with `Int` is a type error. This checks ordinary source
  typing, not the future migration entry checker.
- A child compiler can check the real task queue with a 1 GiB address-space limit,
  a 10-second CPU limit and a 15-second wall deadline on this Linux host.
- Excessive syntactic nesting is refused within those limits.

Run from the repository root:

```
python3 learnability/phase5/preflight/probe.py build/gcc/debug/mo
```

The JSON output includes the executable digest and timings. These are feasibility
observations, not a live-pause bound. This probe deliberately changes no compiler
or runtime code. The native transfer and schema-synthesis experiments below extend this initial
probe. Production activation correctness still needs implementation evidence. A successful ordinary `mo check` must not be reported as that evidence.

Before the native transfer probe: predict that a constructed task-queue-shaped
5,000-job value with 2,048-byte payloads fits 64 MiB in each of two heaps;
encode/decode, one compiled identity call, re-encode and a durable snapshot
commit complete in under one second on this host. The source declarations are
copied from the queue but compiled as Main, so nominal names differ. This is
not yet the complete activation transaction or a worst-case queue of callers.

## Native transfer and synthesized declarations

```
make CC=gcc
python3 learnability/phase5/preflight/run-transfer.py
```

`run-transfer.py` links only the completed debug build's ordinary objects, in a
private temporary directory. It does not change the production binary. The
native prototype compiles a task-queue-shaped service, constructs 5,000 separate
2,048-byte job payloads and 500 leases, and measures encoding, decoding into a
second process, a compiled identity function, re-encoding and decoding for
validation, copying 64 15,000-byte queued arguments, and durable writes. It first
uses the real store snapshot operation, then a second synced SQLite transaction
retains the old snapshot and two maximum-size bundles. This overcounts syncs,
but simplifies metadata columns and does not implement a production atomic
activation. It is a sizing experiment, not a transaction-correctness proof.

`transfer-results.json` records five debug runs: 10,464,313 encoded bytes,
23,568,104 old-heap peak bytes, 12,565,784 new-heap peak bytes, and one identity
instruction. The complete measured interval was 0.239–0.270 seconds. The 1-second
proposed target has a 3.7x margin over the slowest observation. Native codec
walks are outside that instruction counter; do not report it as total CPU work.
The fixture models the largest job/payload counts; it is not an exhaustive
search of all map shapes, histories, arrival queues or timer phases. The
source's nominal prefix is Main rather than JobQueue, accounting for a small
encoded-size difference from the earlier Phase 4 fixture. This is a debug-only
measurement on the current Linux VM/filesystem, not a portable upper bound.

The same executable uses the existing public query-entry checker for three
controls: a pure one-old-state-parameter function is accepted; a different
first type and an extra Console capability are refused. This validates reuse
of those checks; checking the exact new result type still belongs to the new
migration entry implementation.

`synthesize.py` reads the compiled image's emitted saved schema, reconstructs
its concrete declarations and compiles them again. The reconstructed schema is
byte-identical; an added field changes its digest. It also emits the declarations
under UpgradeOld, compiles the migration to bytecode and executes it, returning
the added revision value 1. The old-source route in `probe.py` independently
extracts the original queue declarations. Both routes therefore work for the
queue. The fixture covers Map instantiation; recursive nominal declarations,
generic declaration reconstruction and production name-override decoding remain
later implementation tests. No language-rule change was needed for this case.

## Failed assumptions and limits kept

The first native probe assumed source structs were runtime records and crashed
when dereferencing a NULL record-fields pointer. Debugging showed Queue is a
single-case runtime choice. The fixed probe uses choice fields. Saved schemas
do not distinguish a struct from a same-named one-case enum; the synthesizer uses
the queue's constructor-name convention. General synthesis must preserve schema
shape while documenting its generated source interface, and use service shape
metadata when available. This cannot be generalized into a claim that the type
table preserves all original source declarations.

The first entry-control fixture returned its Int state directly as a service
tool, which the checker correctly refused before entry selection. It now uses
a Bool reader; this prevents that unrelated diagnostic from masquerading as a
migration refusal.

Compilation observations in `results.json` include a real task-queue service
bytecode compilation and 64 distinct nested List instantiations under the child
resource limits, plus the over-nested rejection control. Neither input family
proves a worst-case front-end bound. Resource enforcement against a deliberate
CPU hog and address-space exhaustion is not covered by these compiler probes.

Both gcc and clang accepted transfer.c with C17, -Wall, -Wextra, -Wconversion,
-Wsign-conversion, -Wshadow, -Wswitch-enum and -Werror. clang-format 14 was applied.
Independent reproduction and repository checks are owned by the integrating
agent. These observations do not establish the production swap or its crash
safety; the model, fault injection and kill loop remain required.

## Independent reproduction

The integrating agent reran both commands after the author handed off the
prototype. `independent-probe-results.json` and
`independent-transfer-results.json` preserve the outputs. All schema and entry
controls passed again; five transfer intervals were 0.231–0.277 seconds. This
reproduces the fixture result (at least 3.6x below the proposed one-second
target), not a separate implementation or a general latency proof.

The independent review exposed a false positive in the initial migration run:
`mo run migration.bc` parsed bytecode as source and returned E0008/status 1,
which the probe mistook for the intended revision value. The corrected probe
runs the synthesized source through the normal compile-and-run pipeline and
requires both exit 1 and empty stderr. Earlier JSON files retain the failed
assertion's claimed result for audit; only `reviewed-transfer-results.json`
provides execution evidence under the corrected check. Bytecode emission is
still separately checked.

The final migration control uses a populated job map and checks that jobs and
`next_id` survive while `revision` becomes 1. The refusal controls now require
status 1 and their specific E0077/E1006 diagnostics; unrelated failures cannot
pass them. `reviewed-probe-results.json` records the stricter compiler controls.

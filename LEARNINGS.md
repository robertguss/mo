# Learnings

## MO-160 protocol image loading

- Json has implicit library dependencies even though it has no `use` lines:
  its parser and writer need List, Option and Result. The embedded protocol
  sources include those modules; the lowering test checks their exact bytes
  against `lib/` and executes the compiled Json parser and writer.
- `mo_load_program_sources` consumes a nonempty, already populated source set.
  Source zero is the entry; library flags correspond to source indices. It loads
  only that closed set, with no filesystem fallback. The caller retains both
  sources and arena until all consumers of the returned module set finish.
- `mo_protocol_build` returns an owned, loaded but uninitialized image. Its
  sources are embedded; private runtime modules are not library modules. The
  builder checks the header entry points and their full structural type shapes
  before returning the image, including field and case order. Callers follow
  each function's loaded parameter ownership when transferring cells.
- The `MO_LIMIT_MCP_HEADER_*` bounds are experimental measurement targets,
  not frozen defaults. Header scanning retains only delimiter state and byte
  counts between fragments. Header parsing returns spans into native ingress;
  it does not interpret body framing or turn field values into Text or Bytes.
- The first header measurement rejected 16 KiB: scanning plus parsing a
  64-field header with a 256-byte token took 2,579,101 VM instructions before
  authority/authentication checks. At 2 KiB those stages took 357,021; the
  experimental header ceiling was lowered accordingly, retaining the 1M
  instruction budget. Full pre-auth work and peak memory still need measurement.
- Source structs lower to one-case choices, not `MO_TYPE_RECORD`. A nested
  choice payload can require a boxed wrapper. Bridge code must validate the
  actual types and case shapes rather than assume C record layouts.
- Heap value allocations already wipe before release, but that does not cover
  VM stacks or native scratch arenas. Mark a protocol process confidential
  before allocating values or calling it. Its stack and scratch then use
  confidential arenas; VM operand arrays and the process structure are wiped
  too. This does not suppress defect rendering: the adapter must still keep
  protocol defects out of rendering and variable visitors.
- Confidential arenas avoid `realloc`, which could release an unwiped old
  allocation. Moved blocks are cleared; their chunks stay charged to the arena
  until destruction. Capacity planning must include those retained chunks,
  not just the newest live buffer. Ordinary arenas keep their existing growth
  behaviour. The allocation-boundary test checks complete chunk wiping on
  success, allocation failure and instruction-budget stops; it is not the
  full protocol canary/deadline test.
- For valid JSON, counting opening brackets/braces and commas outside strings
  gives `nodes - 1 + empty containers`, where nodes are JSON values and object
  keys are not separate values. This is not an exact semantic-node count.
  The approved native prescan only returns that marker count or a byte-budget
  refusal; JSON validity and the eventual node admission policy remain in Mo.
  The body byte ceiling is experimental, like the header ceilings.

## MO-159 simulation boundaries

- `mo_service_send` returning `MO_SERVICE_MEMORY` means admission failed before
  queueing. The same status in a reply means the handler ran and failed, which
  counts toward D308. Classify by origin, not status alone. Updating messages
  have an aggregate saved-byte limit in addition to each value's codec limit.
- Core simulation controls need no service descriptors. Only `start` and `send`
  select descriptors. Requiring a table for every operation prevents checking a
  standalone helper module, even when source checking accepts its capability.
- A NULL-path store is an in-memory database whose identity lasts until close.
  Simulated crash must keep that same open store. Opening another one tests a
  fresh service, not recovery. The capability releases the loop before the store.
- Lean checks simulation operation types but does not model services. Exclude
  only runs that reach its explicit boundary stop; keep ordinary runs from the
  same program in differential comparison.
- Recovery and snapshot failures have no start-handler attribution. Set a
  handler identity only while invoking that handler or accepting its value;
  replay supplies its own identity. A stopped startup is not necessarily a
  failed initializer.

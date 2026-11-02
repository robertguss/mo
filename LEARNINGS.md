# Learnings

## The task-queue service

- Whatever `submit` accepts, `lease` has to return. A reply carries a payload
  twice, the second copy escaped again, so a byte costs 2 to 13 bytes of the
  32 KiB limit. Past that, `lease` commits and then fails with `response_limit`,
  and the job goes Dead with no worker having seen it. 2,048 bytes fits whatever
  the bytes are. A page holding that worst case can exceed the limit at `limit`
  4, so clients halve `limit`, and 1 always fits.
- Spec code is compiled into the served image and debug spans are in its key, so
  a test inside a service's own file freezes with it. A sibling test entry with
  `use Main` keeps tests out of the image. A service used across modules needs
  `expose` for its wire types, and a `use` list names a choice type, not its
  cases (E0044).
- Every update re-encodes the whole state. Filling the queue to 5,000 jobs of
  empty payloads takes about 90 seconds in the gcc debug build, so the suite's
  fill stays short-payload.
- The image key covers library code too. `mo serve` reads `lib/` from the path
  set at build time, so a comment added to `lib/list.mo` in that checkout refuses
  a service's saved data at its next start, with the service and the binary
  unchanged.
- `mo serve` has no fake clock. Lease expiry is proved in Simulation; the loopback
  check covers wire shapes, size limits and recovery after `kill -9`.

## MO-182 image key

- Debug file paths reach only the `files` section. Emitted bytecode for an
  absolute path, `sub/../counter.mo` and a symlinked folder differs from
  `counter.mo` in no other row, sibling modules included, so the service image
  key hashes the file without that one section. `lib/` rows hold the absolute
  `lib/` path, so the old key also moved with the toolchain's folder.
- Only the reader knows wire offsets, and it keeps no bytes after decoding, so
  it computes `program_checksum` itself. That puts the key inside the `read.c`
  proofs: `sections` proves the bounds that `decode` hashes between.
- Debug spans stay in the key, so an edit that moves code changes it. Comment
  and whitespace edits that move nothing do not, and an edit after the last
  `end` depends on the program: an entry module with no `main` has a debug row
  spanning the whole file, so it is refused, while a sibling module or a module
  with `main` recovers. The guide therefore promises neither way.
- Replay also refuses an edit that changes saved state, because it reruns each
  logged message and compares a state checksum. `state * amount` in the state
  slot stayed refused with a broken key, for that reason. A test of the key has
  to change only a reply, a start value or a description.

## MO-161 mo serve

- The service image key hashed every byte of the bytecode, debug file paths
  included, until MO-182. An absolute path or `sub/../counter.mo` was refused.
- The stop signal handlers go in before the store opens. A signal during the
  first open would otherwise kill the process mid-commit and leave a zero-byte
  `service.sqlite`, which later starts refuse as damaged.
- A diagnostic's `reason` prints only when its code has a section, and
  `tests/test_codes.c` keeps tools codes section-free. E1409's citation of 11.5
  is therefore in its label.

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
  builder checks the header and catalog entry points and their full structural
  type shapes before returning the image, including field and case order and
  recursive Json identity. Callers follow each function's loaded parameter
  ownership when transferring cells.
- The private protocol ABI sees lowered field names: source enum payloads use
  `"0"`, while builtin Option and Result payloads are unnamed. That ABI check
  is not service shape inference. Service schemas use the explicit shape
  table, including its payload flags, and copy names into the protocol heap
  so catalog generation does not borrow the service image.
- The `MO_LIMIT_MCP_HEADER_*` bounds are measured defaults: D329 froze the
  1 KiB header block with its 1M-instruction and 1 MiB memory budget. Header
  scanning retains only delimiter state and byte counts between fragments.
  Header parsing returns spans into native ingress; it does not interpret body
  framing or turn field values into Text or Bytes.
- History: the first header measurement rejected 16 KiB: scanning plus parsing a
  64-field header with a 256-byte token took 2,579,101 VM instructions before
  authority/authentication checks. At 2 KiB those stages took 357,021; the
  experimental header ceiling was lowered accordingly, retaining the 1M
  instruction budget. D329's matrix then lowered it to 1 KiB. Bytewise
  pre-authentication there took 482,877 instructions and 75,862 bytes, inside
  the 2x thresholds of 500,000 and 524,288 (the MO-160 part 2 round E report, in
  PR #262's history).
- Source structs lower to one-case choices, not `MO_TYPE_RECORD`. A nested
  choice payload can require a boxed wrapper. Bridge code must validate the
  actual types and case shapes rather than assume C record layouts.
- Preserve source struct/enum form and unnamed-payload flags while lowering,
  before the Core representation loses those distinctions. The service
  sidecar owns copies in the lowering arena and indexes them by Core data
  order. Built-in Entry is a physical record, not a source data declaration;
  its descriptor shape must be supplied separately during code generation.
- Per-service shape traversal repeated large shared structural subgraphs even
  when they contained no nominal types: 4096 services over one 65535-field
  tuple caused 268431360 edge visits. Keeping one global visited set would
  lose per-service completeness. Shared component analysis with 64-service
  membership batches preserves isolation without allocating the full
  types-by-services reachability matrix; its work bound includes batch count.
- Heap value allocations already wipe before release, but that does not cover
  VM stacks or native scratch arenas. Mark a protocol process confidential
  before allocating values or calling it. Its stack and scratch then use
  confidential arenas; VM operand arrays and the process structure are wiped
  too. This does not suppress defect rendering: the adapter must still keep
  protocol defects out of rendering and variable visitors.
- Confidential storage alone does not combine stack and heap limits. Call
  `mo_process_account_stack` before execution to charge retained stack chunks
  through the process heap, alongside native `mo_scratch` buffers. The process
  memory counters include its native structure; instruction remaining is
  cumulative across calls until explicitly reset. A nearly full native scratch
  buffer plus a small scalar-returning Mo call distinguishes a charged stack from
  an uncharged one without relying on private VM layouts.
- Confidential arenas avoid `realloc`, which could release an unwiped old
  allocation. Moved blocks are cleared; their chunks stay charged to the arena
  until destruction. Capacity planning must include those retained chunks,
  not just the newest live buffer. Ordinary arenas keep their existing growth
  behaviour. The allocation-boundary test checks complete chunk wiping on
  success, allocation failure and instruction-budget stops; it is not the
  full protocol canary/deadline test.
- Final chunk wiping can hide a missing move-time or shrink-time wipe. Check
  discarded scratch bytes immediately after resizing, before freeing the
  arena, and keep live prefixes and neighboring allocations as controls.
- CBMC loop IDs do not follow source nesting order. In `service_entry`, loop
  `.2` is the inner four-byte case loop and `.3` is the outer twelve-byte
  shape loop. Check `goto-instrument --show-loops` before assigning byte-based
  unwind bounds; swapped divisors fail the inner loop's unwinding assertion.
- For valid JSON, counting opening brackets/braces and commas outside strings
  gives `nodes - 1 + empty containers`, where nodes are JSON values and object
  keys are not separate values. This is not an exact semantic-node count.
  The approved native prescan only returns that marker count or a byte-budget
  refusal; JSON validity and the eventual node admission policy remain in Mo.
  D329 froze the 16 KiB body byte ceiling as a measured default, like the
  header ceilings.
- JsonNumber is opaque outside Json. Its compact JSON rendering preserves the
  original numeric token, so protocol Byte decoding can inspect that spelling
  without a new accessor or a Float conversion. An all-zero mantissa is zero
  even with a huge exponent; a nonzero token must prove both integrality and
  range before conversion.
- BigInt text input uses nine-digit chunks over the existing arithmetic, not
  a new primitive. The 4096-digit target is experimental: isolated GCC O2
  decoding used 373001 VM instructions and about 6 ms in the initial probe.
  That result does not freeze an aggregate request budget. Native copying,
  Json parsing, collection sizes and concurrent connections still count.
- JSON Schema 2020-12 requires `prefixItems` to be nonempty. An empty tuple
  uses `minItems: 0`, `maxItems: 0` and `items: false` without `prefixItems`.
  An empty enum uses the false schema, not an empty `oneOf`. Validate generated
  schemas against the metaschema as well as validating example values.
- D321's private Time bridge clarification permits `TimeValue(Time)` so Mo
  retains parsing and canonical formatting without a new public constructor.
  C copies only signed microseconds into a fresh cell. Duration remains an
  Int payload in microseconds; neither conversion transfers image-local types
  or heap references. Temporary Bytes conversion storage uses `mo_scratch_resize`
  so it shares the destination heap budget.
- Composite conversion must use the service image's typed equality for Map
  keys and Set elements. Different accepted wire spellings can become the same
  value, so JSON equality cannot enforce uniqueness. Destination table
  cardinality detects that collapse after typed conversion. Product construction
  uses the heap's indexed layout lookup rather than scanning layouts per value.
- D322's RPC classification checks the version header after parsing an object
  but before validating its envelope, except for an exact string `initialize`
  method. Parse errors precede that check. Numeric RPC IDs keep their lexical
  spelling rather than using the mathematical Byte or tool-Int decoders.
- Pagination admission must include the RPC envelope, the worst-case escaped
  ID and the next cursor. Checking only a tool's schema size can admit a catalog
  that cannot advance for a valid request. Cached serialized tool sizes avoid
  rendering every growing page prefix to choose its boundary.
- Service wire visibility is not handler-signature visibility. Skip State and
  Time, remove only the outer Result and designated updating pair, then check
  arguments, reply and application error. Run those wire checks after E0101
  across all services so a hidden type or predicate name cannot mask the
  approved bare-state diagnostic.
- D317's instruction allowance is not native C gas: one primitive dispatch is
  one instruction. Bound native byte/node/iteration work and measure its stage
  time separately. An invented quadratic comparison reservation can reject
  valid Map/Set inputs before the runtime's typed duplicate check.
- Prepared descriptor and catalog graphs must not be rebuilt per request.
  Freeze the immutable graphs, including their hash caches, during server
  construction and keep the owning heap alive until every request is freed.
  Ordinary refcounted cells cannot be shared across process heaps this way.
- A post-call live-memory sample misses transient scratch. Track aggregate
  scratch reservations in the heap, including simultaneous arenas and headers.
  A sum of active request lifetime peaks is a conservative concurrent bound,
  not a measured simultaneous-live high-water mark.

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

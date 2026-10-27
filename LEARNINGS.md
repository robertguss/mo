# Learnings

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

---
name: blast-radius
description: Determine what else a small-looking Mo change could break and prove the safety claim by running code, not by asserting it.
---

# Blast Radius (Mo)

Adapted from pstack `blast-radius` by poteto/Lauren Tan (MIT, via cursor/plugins) for Mo.

## Method

1. **Name the change and its surface.** Which module (`src/{base,bytecode,core,heap,tools}`), which public header, which `.def` table rows, which callers. `make deps` shows allowed includes; search every call site of each changed symbol, including re-lookups and re-stores through aliases.
2. **Enumerate what could break**, by layer: C callers and tests (`tests/test_*.c`), the Core text oracle shared with Lean (`formal/TEXT.md`), the executable grammar (`grammar/mo.y`), fuzz targets and corpora, the library map (`docs/design/library.md` vs the `.def` tables), and downstream examples (`examples/*.mo`).
3. **Prove safety by running, per claim.** Each "safe because X" must be an executed check: the named test suite, `make grammar`, `make formal`, `make library-map`, `make deps`, a fuzz smoke run, or a `mo` CLI invocation — with its actual output. An asserted safety fact without a run is a hypothesis; label it as such.
4. **Check the reused-handle chain.** For shared/aliased state (arena blocks, cells, buffers), trace the identifier through every alias and re-store in callers, and test one chain where a reused handle is stored again under its reused name.

**Output:** the change, the affected surface, claim-by-claim evidence (command + result), and any claim left unverified with the reason.

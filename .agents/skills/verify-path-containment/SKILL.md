---
name: verify-path-containment
description: Prove that Files path normalization refuses escapes. Use when changing mo_host_normalize, src/host/files.c, a Files/ReadFiles path argument, sandbox-properties P8, or cbmc/path.c.
---

# Verify path containment

A `Files` or `ReadFiles` path that leaves its folder is a sandbox escape. `mo_host_normalize` in `src/host/files.c` is the lexical gate both real and fake Files operations run before they open or look up anything. This skill checks that gate against architecture section 7 ("Files") and sandbox-properties P8.

**Property:** for every path of up to 6 bytes over `{'.', '/', 'a'}`, `mo_host_normalize` accepts exactly the contained relative paths: split on `/`, skip `.`, pop on `..`, refuse an empty path, an empty component (`/a`, `a/`, `a//b`), or a `..` that would leave the root (`..`, `../a`, `a/../..`). An accepted path is the spec's bytes; a refusal is `FILE_NOT_ALLOWED` with `containment`.

**Harm if it breaks:** a Mo program with a folder capability can name a path above that folder.

## Does this change need the check?

Run it when the diff touches any of:

- `src/host/files.c` (`mo_host_normalize`, or the `MO_CBMC_NORMALIZE` guards)
- `src/host/files.h` (the `mo_host_normalize` signature)
- `src/host/real-files.c` or `src/host/fake-files.c` (a path argument that used to call `mo_host_normalize` and no longer does)
- `src/host/path.c` (only if it starts duplicating lexical rules instead of taking normalize's result)
- `cbmc/path.c` or the `normalize` row in `cbmc/prove.sh`
- `docs/design/sandbox-properties.md` P8, or architecture section 7 "Files" path wording

A change that adds a Files operation taking a path, and does not call `mo_host_normalize` on that path, is a coverage hole: the proof can still pass. Report that; do not treat a green proof as P8 holding.

If none of those files changed, stop.

## Run

Needs CBMC (`cbmc`, `goto-cc`, `goto-instrument`). `.agents/setup cbmc` installs it on the Linux orb; on macOS, `brew install cbmc`.

```sh
bash cbmc/prove.sh normalize
```

About 2 seconds. A pass prints `proved normalize`. A fail prints `FAILED normalize: see build/cbmc/normalize.log` and the assertion that broke.

When the body of `mo_host_normalize` gains a branch, also run:

```sh
bash cbmc/prove.sh --reach normalize
```

About 20 seconds. That fails if a block of `mo_host_normalize` never runs on any input in scope, so a new check is not silently unused.

`make cbmc` runs this proof with every other CBMC proof. Use that when the change also touches the reader, arena, verifier, or primitives. Do not use it as the tight loop for a path-only change.

## Pass or fail

**Pass** covers the bound below, not the whole resolver and not paths longer than 6 bytes.

**Fail:** open `build/cbmc/normalize.log`. Replay the counterexample as a path a Mo program could pass to a Files operation. Walk it through `mo_host_normalize` the way the C does (one component per `/`). Then:

- If every step can happen in the current `mo_host_normalize`, it is a **bug**. State the path, what the function returned, and that a folder capability would accept an escape or reject a contained path. Do not weaken `cbmc/path.c` to make the proof pass. Do not change `src/host/files.c` unless the user asks.
- If a step cannot happen in the C (the spec treats a byte as special that the C copies as a name, or the other way around), it is a **modeling error**. Fix `cbmc/path.c` so each step is one step in `mo_host_normalize`, and rerun.

The proof was shown able to fail: deleting the `written == 0` refusal on `..` makes `..` accepted as the root. `bash cbmc/prove.sh normalize` then fails `normalize: refuses an empty, absolute, or escaping path`. Restore that refusal; the check must go back to passing.

## Bounds and assumptions

- **Bound:** length at most `MO_CBMC_PATH` (6). Raise it in `cbmc/path.c` and the unwind set on the `normalize` row of `cbmc/prove.sh` if you need a longer path; time grows with it.
- **Alphabet:** bytes are `.`, `/`, or `a`. `a` stands for any other component byte. The C only special-cases `.` and `/`.
- **Heap:** `mo_value_data` is a model that returns the symbolic path. The proof does not run the heap.
- **Callers:** the proof is of `mo_host_normalize` itself. NUL in a path is refused by `src/host/real-files.c` before this function; the portable and `openat2` resolvers in `src/host/path.c` are a second gate, not this one.
- **Not covered:** link targets, `..` after a resolved directory, concurrent directory replacement (P8's documented gap), paths longer than the bound.

## Coverage and sync

| Check | Code | Last confirmed |
| --- | --- | --- |
| `normalize` | `mo_host_normalize` in `src/host/files.c`; spec and harness in `cbmc/path.c`; callers `mo_host_real_files_call` and `call` in `src/host/fake-files.c` | `85853e8` (MO-58). Confirm again after this skill's proof files land, then move the hash. |

Move the sync commit only after `bash cbmc/prove.sh normalize` passes on that commit and the spec still matches the C: one component per `/`, one pop per `..`, one refusal per empty or escaping component. A pass on a stale description is the dangerous case; `/maintain-formal-verification-skill` is the upkeep loop when commits skipped this skill.

# Content digests for promises and example rows: feasibility prototype

A throwaway prototype of the planned identity rule: each promise and each example row
gets a SHA-256 digest over its owner, its kind, its own text and the sorted name and text
of every declaration its checked body reads. It runs on `services/task-queue` at commit
02822786. It answers three questions: which declarations each item reads, whether the
formatter can make comments and layout drop out of the digest, and what including the
subject's signature changes.

Nothing in `src/` changed, so there is no `prototype.patch`. The prototype is a standalone
C program, `identity.c`, linked against the debug build's objects. It calls `mo_check` and
walks the checked program it returns.

## Files

| File | What it is |
| --- | --- |
| `identity.c` | The prototype. Usage: `identity ENTRY.mo LIBRARY-FOLDER`. Prints JSON. |
| `run.sh` | Builds `mo` and the prototype, digests the service and each variant, runs `mo test` on each, and writes `out/`. Takes about 10 minutes, because each debug `mo test` run takes about a minute. |
| `compare.py` | Compares each variant's JSON with `out/base.json` and prints `out/table.md`. |
| `variants/*.patch` | The variants, as unified diffs against `services/task-queue`. `run.sh` applies each one to a fresh copy under `build/identity/runs/`. |
| `out/<variant>.json` | Raw prototype output: each item with its name, kind, subject, line, reach, digests for three text rules × two flavours, canonical own text and subject signature, plus the text of every declaration reached. |
| `out/<variant>.test.txt` | The `mo test` exit status, the name of each failed check, and the summary line. |
| `out/table.md` | The variant tables below, as `compare.py` printed them. |

To reproduce everything, run `bash learnability/phase6/preflight/identity/run.sh` from any
clean checkout of 02822786 that has this folder.

## What the prototype computes

For each `MO_SPEC_PROMISE` and `MO_SPEC_EXAMPLE` in `mo_program.specs` outside library
modules:

- **Reach.** The walk starts at the item's checked body; a promise also includes its
  allows/forbids anchors. Every expression row's `symbol` (a function, constant or
  constructor, where a case maps to its enum) and every user type in a row's or binding's
  type is visited. A visited function contributes its signature type and checked body. A
  constant contributes its type and initializer. A struct or enum contributes its field
  types. Any custom `to_text`/`compare` proofs are followed too. The walk stops at library
  modules and built-ins, and skips the subject's declaration at every depth. Specs are
  never walked: a helper's own promises and examples are not part of another item's reach.
  This is the same walk `src/check/recursion.c` does over checked bodies, done here from
  outside the checker.
- **Declaration text.** The declaration is found again in a fresh, unchecked parse of its
  file, matched by span and kind. A function's text leaves out its promise and examples
  blocks, so no digest contains another item's text. Three text rules are computed:
  - `raw`: exact source bytes, with the spec blocks cut out.
  - `formatted`: `mo_format` output for a synthetic file holding only that declaration.
  - `canonical`: the same, after clearing every comment anchor (`leading`, `trailing`,
    `ending`) and every `blank_before` bit in a copy of the subtree.
- **Digest.** SHA-256 over length-prefixed fields, in this order: owner (`Module.subject`),
  kind, own text (the whole promise block including anchors, or the one example row), the
  subject's signature (the `with_signature` flavour only), then name and text of each
  reached declaration in sorted order. The subject's signature is the function printed
  with its specs and body removed, for example
  `fn heartbeat(queue: Queue, now: Time, id: Int, attempt: Int) : Result(Queue, QueueError)`.

## Items and their reach (unchanged service)

Command: `build/identity/identity services/task-queue/main.mo $PWD/lib` (output:
`out/base.json`).

There are 19 items, all in `job-queue.mo`. `main.mo` has no promises, examples or service
promises. Every item's reach includes `Job`, `JobState` and `Queue`; the table lists the
rest.

| Item | Reach besides Job, JobState, Queue |
| --- | --- |
| submit.promise.1 | empty, holding, ready |
| lease.promise.1 | QueueError, empty, holding, leased, ready, t0 |
| lease.example.1, .2 | QueueError, empty, t0 |
| heartbeat.promise.1 | QueueError, holding, leased, t0 |
| heartbeat.promise.2 | QueueError, holding, leased, ready, t0 |
| acknowledge.promise.1 | QueueError, done, holding, leased, t0 |
| acknowledge.promise.2 | QueueError, done, holding, leased, ready, t0 |
| fail.promise.1 | QueueError, holding, retried, t0, tried |
| fail.promise.2 | QueueError, dead, holding, **max_attempts**, retried, t0, tried |
| fail.example.1 | QueueError, holding, retried, t0, tried |
| fail.example.2 | QueueError, dead, holding, t0, tried |
| fail.example.3 | QueueError, holding, ready, t0 |
| fail.example.4 | QueueError, empty, t0 |
| expire.example.1 | holding, leased, retried, t0 |
| expire.example.2 | holding, leased, t0 |
| jobs.example.1 | empty |
| jobs.example.2 | done, holding, ready |
| jobs.example.3 | QueueError, **fail, leased_job, max_attempts, put, release**, holding, ready, retried, t0, tried |

### The two required checks pass

- `JobQueue.jobs.example.3` (`fail(holding([tried(1, 1), ready(2)]), t0, 1, 1).map(jobs) == ...`)
  lists `fail` and everything `fail`'s body reads: `leased_job`, `release`, `put`,
  `max_attempts`, `QueueError`. `jobs`, its subject, is not listed.
- `JobQueue.fail.promise.2` ("a job is attempted at most three times") lists
  `max_attempts`, because its condition reads it directly.

### How many items read a body other than their subject's

- **Operation bodies.** 1 of 19 items, `jobs.example.3`, reads one: `fail`, `leased_job`,
  `release` and `put`.
- **Fixture builders.** 15 of 19 read the body of at least one of `holding`, `ready`,
  `leased`, `tried`, `retried`, `done` and `dead`. These are plain functions that build
  test values.
- **No functions.** 4 items read no function at all: lease.example.1 and .2,
  fail.example.4 and jobs.example.1.

Which items an ordinary body fix makes change, counted from `out/base.json`:

| Fix to | Items that change |
| --- | --- |
| Anything in `main.mo` (tool bodies, constants, types, tool parameters) | 0 |
| The body of an item's own subject (`lease`, `heartbeat`, `acknowledge`, `expire`, ...) | 0 for that subject's items |
| `fail` body, `leased_job`, `release`, `put` | 1 (jobs.example.3) |
| `max_attempts` | 2 (fail.promise.2, jobs.example.3) |
| `dead` / `done` / `retried` / `tried` / `ready` / `leased` | 2 / 3 / 5 / 5 / 7 / 7 |
| `empty` | 6 |
| `QueueError` | 14 |
| `holding` | 15 |
| `t0` | 16 |
| `Job`, `JobState`, `Queue` | 19 (all) |

So ordinary fixes to the queue's logic leave approvals alone, except the one example row
that goes through `fail`. Editing a fixture builder or a core type touches most approvals.

## Comments and layout

**Can the formatter print a single declaration?** Yes. `mo_format` takes a
`mo_module_syntax`, and only its tree is read (`format.c` uses no source storage beyond
leaf and comment text). The prototype wraps one declaration in a synthetic `MO_S_FILE`
node with no header and gets that declaration's text back. No change to `src/` was needed.

**Does formatted text ignore comments and layout?** Formatting alone handles only part of
it:

- Line breaks and indentation inside a statement are dropped, because the formatter
  chooses breaks from the tree.
- Comments are kept, because they are anchored in the tree (`leading`, `trailing`,
  `ending`), and so are single blank lines (`blank_before`).
- Clearing those anchors and bits in a copy before printing removes both.

Evidence:

- Variant `g-release-comment` adds `# The last attempt makes the job dead; any earlier one
  makes it ready.` inside `JobQueue.release`. The `raw` and `formatted` texts of `release`
  both contain the comment, so jobs.example.3's digest changes under both. The `canonical`
  text equals the unchanged service's, so no digest changes.
- Variant `h-layout-only` adds a blank line inside `release` and splits
  `after.jobs.set(job.id, job)` in `put` over four lines:
  - `raw` changes, so jobs.example.3 changes.
  - `formatted` folds the call in `put` back onto one line but keeps the blank line in
    `release`, so jobs.example.3 still changes.
  - `canonical` equals the unchanged service, so 0 items change.
- `raw` is not even consistent about comments. A declaration's span starts at `fn`, so a
  `##` comment above it is left out, while a `#` comment inside it counts. The `formatted`
  rule includes both.

The exact texts are in `out/g-release-comment.json` and `out/h-layout-only.json` under
`declarations` → `JobQueue.release` / `JobQueue.put`.

**Rule the prototype implements (the `canonical` columns):** a declaration's and an item's
text is the formatter's output for that node alone, after removing every comment anchor and
blank-line bit, and with a function's promise and examples blocks removed. Exact bytes are
never needed for this service, because the formatter printed every declaration it reached.
The planned fallback to exact bytes never ran.

## Variants

Every variant is a patch in `variants/`. Checks are `build/gcc/debug/mo test
<copy>/queue-tests.mo`, which runs all 47 checks: the 19 items, the 4 JobQueue tests, the
QueueTests tests and Main's recovery check.

| Variant | What it changes |
| --- | --- |
| a-handler-fix | `full_advice` text in `main.mo`, and `Main.list`'s `matching.size > limit` rewritten as `limit < matching.size` |
| b-release-fix | `JobQueue.release` body: `if job.attempts < max_attempts: Ready else: Dead` |
| c-max-attempts-4 | `max_attempts = 4` |
| d-heartbeat-swap | `JobQueue.heartbeat(queue, now, attempt, id)`; its caller in `Main.heartbeat` updated, and so are 5 positional calls in JobQueue's tests (needed to keep the checks passing). The promises' anchors are named, so they did not change. |
| d2-fail-swap-main-caller-only | Extra: the same swap on `JobQueue.fail`, updating only the `main.mo` caller |
| d3-fail-swap-every-call | Extra: d2 plus every positional call: 3 example rows and 4 test calls. The rows with `1, 1` stay as they are. |
| e-tool-rename | `Main.heartbeat`'s parameter `attempt` renamed `attempt_number`, which changes the MCP argument name clients send |
| f | Not run. No realistic change exists in the task queue that narrows or changes a parameter type of a promised function. The parameters are `Queue`, `Time`, `Duration`, `Text` and `Int`, and Mo has no narrower subtypes of them. Swapping one for a new type would also change anchors or rows. |
| g-release-comment | a `#` comment inside `JobQueue.release` |
| h-layout-only | a blank line in `release`, and the call in `put` split over lines |

Canonical digests (the rule above):

| Variant | `mo test` | Changed, signature included | Changed, signature left out |
| --- | --- | --- | --- |
| a-handler-fix | 47 passed | 0 | 0 |
| b-release-fix | 47 passed | 1: jobs.example.3 | 1: jobs.example.3 |
| c-max-attempts-4 | refused: `error E1405: a forbids anchor was accepted job-queue.mo:185:5` | 2: fail.promise.2, jobs.example.3 | 2: fail.promise.2, jobs.example.3 |
| d-heartbeat-swap | 47 passed | 2: heartbeat.promise.1, heartbeat.promise.2 | 0 |
| d2-fail-swap-main-caller-only | 41 passed, 6 failed (fail.example.2–4, JobQueue.test.1, .3, .4) | 7: fail.promise.1–2, fail.example.1–4, jobs.example.3 | 1: jobs.example.3 |
| d3-fail-swap-every-call | 47 passed | 7: fail.promise.1–2, fail.example.1–4, jobs.example.3 | 4: fail.example.2–4, jobs.example.3 |
| e-tool-rename | 47 passed | 0 | 0 |
| g-release-comment | 47 passed | 0 | 0 |
| h-layout-only | 47 passed | 0 | 0 |

Count of changed items under each text rule (signature included / left out). No variant
changed any item's reach list.

| Variant | raw | formatted | canonical |
| --- | --- | --- | --- |
| a | 0 / 0 | 0 / 0 | 0 / 0 |
| b | 1 / 1 | 1 / 1 | 1 / 1 |
| c | 2 / 2 | 2 / 2 | 2 / 2 |
| d | 2 / 0 | 2 / 0 | 2 / 0 |
| d2 | 7 / 1 | 7 / 1 | 7 / 1 |
| d3 | 7 / 4 | 7 / 4 | 7 / 4 |
| e | 0 / 0 | 0 / 0 | 0 / 0 |
| g | 1 / 1 | 1 / 1 | 0 / 0 |
| h | 1 / 1 | 1 / 1 | 0 / 0 |

## What including the subject's signature changes

- **d (heartbeat swap, all checks pass).** With the signature included, both heartbeat
  promises change and would wait for re-approval. Left out, nothing changes. Neither
  promise says anything different: conditions read `id` and `attempt` by name, and anchors
  bind by name (`id: 1, attempt: 1`). So including the signature asks for a re-approval of
  unchanged meaning.
- **d3 (fail swap, every call updated, all checks pass).**
  - With the signature left out, exactly the rows whose written text changed are flagged:
    fail.example.2–4 (their arguments were swapped), and jobs.example.3 (it reads `fail`
    as a non-subject declaration, signature included).
  - fail.example.1 (`t0, 1, 1`) and both fail promises stay. Their text and meaning are
    the same.
  - With the signature included, all 7 are flagged.
- **d2 (fail swap, rows not updated).** Leaving the signature out keeps the 4 fail
  examples' digests the same although 3 of them now mean something else positionally.
  The checks catch it (6 checks fail), and fixing the rows changes their text and so
  their digests, as d3 shows.
  - The gap would show only for a positional row that still passes after a swap with
    unequal arguments. No such row exists in the task queue.
- **e (tool parameter rename).** No digest changes in either flavour. No item is about a
  `Main` tool, and no item reaches `Main`. A change to the MCP interface clients call is
  invisible to item identity in this service, whatever is decided about signatures.
- **c (constant).** Both flavours agree.
  - fail.example.2 (`fail(holding([tried(1, 3)]), t0, 1, 3) == Ok(holding([dead(1)]))`)
    keeps its digest although its outcome depends on `max_attempts`. It reads
    `max_attempts` only through its subject's body, which the rule leaves out. That is the
    rule working as written; here `mo check` refuses the program anyway (E1405 on
    fail.promise.2's forbids anchor).

In this service, the subject's signature never caught a change in an item's meaning that
the checks or the item's own text did not catch. It did add re-approvals for meaning-
preserving reorders.

## Not covered or not verified

- **Service promises.** The task queue has none, so including the signatures of the tools
  a service promise's steps range over was not exercised.
- **Type and result-type changes.** No variant for f. A promised function's parameter or
  result type changing was not measured.
- **Overloads.** Owner names are `Module.function`. The receiver suffix `mo test` adds for
  overloads is not included, because the task queue has no overloads.
- **Comment-only changes to a `Main` tool's `##` text.** Not measured as a variant. They
  reach no item, so they cannot change any digest here.
- **c-max-attempts-4.** It stops at the first anchor refusal (E1405), so the example rows
  never ran in that variant. Its digests were still computed, because the prototype only
  needs `mo_check` to accept the program, and it does. Anchor verification runs later, in
  the tool.
- **The prototype's limits.** It uses fixed tables: 64 source files and 512 declarations.
  It asserts rather than reporting an error. It was compiled with `-Wall -Wextra -Wshadow
  -Wconversion -Wsign-conversion -Werror` under gcc only, not clang, sanitizers or
  `clang-format`.

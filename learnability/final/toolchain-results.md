# The final round on the Phase 2 toolchain

MO-121, 24 October 2026. The baseline is main at
[`267370f`](https://github.com/robertguss/mo/commit/267370f50a13e0ce12d103346ae63b6861ba3436).
The results below include MO-121's two lowering fixes. None of the 32 recorded
programs or graders' reports has changed.

MO-136 reran the script on main at
[`fc53746`](https://github.com/robertguss/mo/commit/fc53746413b436a0fe1ca0dc935f77c3a6710314),
after MO-127 (#195) and MO-128 (#194). All 32 originals give the same exit
status and diagnostic as before, so the table's totals are unchanged. One scratch
probe changed: the GPT job-queue copied to `answers/` now passes all five
promises, which closes [L3](#l3-hidden-builder-dependencies-fixed-by-mo-127).
Every difference is now either fixed in the toolchain (L1, L2, L3) or traced to
a later language rule (D228, D232), the collection layout (C1), or a mistake in
a program (P1, P2). No toolchain gap remains open. MO-128 changes no result here:
main at [`96ad7fb`](https://github.com/robertguss/mo/commit/96ad7fb), before
both tasks, gives the same output as
[`a127a55`](https://github.com/robertguss/mo/commit/a127a55), after MO-128 only.

The old Phase 2 note that all 32 programs fail early no longer holds. On the
recorded paths, 25 pass `mo check`. Sixteen pass `mo test`, nine have no
observations to select, and seven stop at diagnostics. The two lowering fixes
do not change these totals because earlier diagnostics hide their affected calls.

Phase 1's verdicts describe the reference at
[`9d24831`](https://github.com/robertguss/mo/commit/9d24831), not today's language.
[Results](results.md), [grader A](gpt-6.1-sol/grade-a.md), and
[grader B](gpt-6.1-sol/grade-b.md) give GPT 14 pass and two close.
Claude's [grader A](claude-code/grade-a.md) and [grader B](claude-code/grade-b.md)
disagree only on job-queue. [Grader C](claude-code/grade-c-job-queue.md)
settles it as close, leaving 15 pass and one close.

## Reproduce

Run from the repository root after building the compiler:

```sh
make -j8 CC=gcc
python3 learnability/final/rerun.py build/gcc/debug/mo > /tmp/mo121-results.json
```

The script runs `check` and plain `test` on every original and captures each
exit status, stdout, and stderr as JSON. It also runs the seven labelled
scratch probes below. Scratch files live in a temporary directory and are
removed afterwards. No probe result replaces an original's result in the table.
The script exits successfully when it records all commands, not when every
program passes. Inspect the recorded exit statuses.

These are compile and embedded-spec results, not a fresh functional grade of
each command-line task. `check` does not run `main`. A program with no examples,
promises, or tests gets exit 2 from `test`, with
`test selection matches no examples, promises or tests`. That is not a test
failure or a toolchain bug. Phase 1 did not require tests for all sixteen tasks.
Passing generated promise trials is not a proof over all inputs.

## All 32 recorded programs

Paths are relative to this directory. `C0` means `check` exits 0. `T0 (N)` means
`test` exits 0 with N passed, zero failed, and zero stopped. `T2` means no
observations. Other results name the diagnostic and exit status. Trace IDs refer
to the explanations below. The baseline, MO-121's fixed compiler, and main at
`fc53746` give the same results in this table.

| Program | Phase 1 verdict | Today's check | Today's test | Trace |
| --- | --- | --- | --- | --- |
| [claude-code/account.mo](claude-code/account.mo) | pass | C0 | T0 (32) | Agrees. |
| [claude-code/expenses.mo](claude-code/expenses.mo) | pass | C0 | T0 (25) | Agrees. |
| [claude-code/find.mo](claude-code/find.mo) | pass | C0 | T0 (29) | Agrees. |
| [claude-code/greet.mo](claude-code/greet.mo) | pass | C0 | T0 (8) | Agrees. |
| [claude-code/inventory.mo](claude-code/inventory.mo) | pass | C0 | T0 (25) | Agrees. |
| [claude-code/job-queue.mo](claude-code/job-queue.mo) | close, B and C; A pass | E0067, exit 1 | E0067, exit 1 | P1. Real pattern error. Scratch repair reveals L2, fixed here. |
| [claude-code/json.mo](claude-code/json.mo) | pass | E0037, exit 1 | E0037, exit 1 | D232. Later reserved module name. Scratch rename passes 19 observations. |
| [claude-code/kv.mo](claude-code/kv.mo) | pass | E0045, exit 1 | E0045, exit 1 | D232. Later reserved `Entry`. Scratch rename passes 49 observations. |
| [claude-code/library.mo](claude-code/library.mo) | pass | C0 | T0 (19) | Agrees. |
| [claude-code/notes-tool.mo](claude-code/notes-tool.mo) | pass | C0 | T0 (28) | Agrees. |
| [claude-code/rate-limit.mo](claude-code/rate-limit.mo) | pass | C0 | T0 (34) | Agrees. |
| [claude-code/roman.mo](claude-code/roman.mo) | pass | C0 | T0 (57) | Agrees. |
| [claude-code/tictactoe.mo](claude-code/tictactoe.mo) | pass | C0 | T0 (24) | Agrees. |
| [claude-code/todo.mo](claude-code/todo.mo) | pass | C0 | T0 (38) | Agrees. |
| [claude-code/wc.mo](claude-code/wc.mo) | pass | C0 | T0 (26) | Agrees. |
| [claude-code/word-freq.mo](claude-code/word-freq.mo) | pass | C0 | T0 (29) | Agrees. |
| [gpt-6.1-sol/account.mo](gpt-6.1-sol/account.mo) | pass | C0 | T0 (29) | Agrees. |
| [gpt-6.1-sol/expenses.mo](gpt-6.1-sol/expenses.mo) | pass | C0 | T2 | Agrees on compilation. No observations. |
| [gpt-6.1-sol/find.mo](gpt-6.1-sol/find.mo) | pass | C0 | T2 | Agrees on compilation. No observations. |
| [gpt-6.1-sol/greet.mo](gpt-6.1-sol/greet.mo) | pass | C0 | T2 | Agrees on compilation. No observations. |
| [gpt-6.1-sol/inventory.mo](gpt-6.1-sol/inventory.mo) | pass | C0 | T2 | Agrees on compilation. No observations. |
| [gpt-6.1-sol/job-queue.mo](gpt-6.1-sol/job-queue.mo) | pass | E0035, exit 1 | E0035, exit 1 | C1. Collection layout. Restored path passes all five promises since MO-127 fixed L3. |
| [gpt-6.1-sol/json.mo](gpt-6.1-sol/json.mo) | pass | E0037, exit 1 | E0037, exit 1 | D232. Scratch module rename checks; no observations. |
| [gpt-6.1-sol/kv.mo](gpt-6.1-sol/kv.mo) | pass | E0045, exit 1 | E0045, exit 1 | D232. Scratch `Entry` rename checks; no observations. |
| [gpt-6.1-sol/library.mo](gpt-6.1-sol/library.mo) | close | C0 | T0 (12) | D228. `Time - Duration` became legal after grading. |
| [gpt-6.1-sol/notes-tool.mo](gpt-6.1-sol/notes-tool.mo) | pass | C0 | T2 | Agrees on compilation. No observations. |
| [gpt-6.1-sol/rate-limit.mo](gpt-6.1-sol/rate-limit.mo) | pass | E0035, exit 1 | E0035, exit 1 | C1. Restored path reveals L1, fixed here, then P2, a wrong test expectation. |
| [gpt-6.1-sol/roman.mo](gpt-6.1-sol/roman.mo) | pass | C0 | T0 (27) | Agrees. |
| [gpt-6.1-sol/tictactoe.mo](gpt-6.1-sol/tictactoe.mo) | pass | C0 | T2 | Agrees on compilation. No observations. |
| [gpt-6.1-sol/todo.mo](gpt-6.1-sol/todo.mo) | close | C0 | T2 | D228. `FileError.to_text` became legal after grading. No observations. |
| [gpt-6.1-sol/wc.mo](gpt-6.1-sol/wc.mo) | pass | C0 | T2 | Agrees on compilation. No observations. |
| [gpt-6.1-sol/word-freq.mo](gpt-6.1-sol/word-freq.mo) | pass | C0 | T2 | Agrees on compilation. No observations. |

## Trace of each difference

### D228 and D232 changed the language after grading

[D228](https://github.com/robertguss/mo/commit/3d0b61f6c6463ee1b87a7cc9a51d3cc4cc7464f6)
added `to_text` for every type with `Text`, and `Time - Duration`. The two GPT
close verdicts were correct against their reference. Today's acceptance is an
approved language change, not evidence of a grader mistake or missing rejection.
The library's existing test at line 173 executes the formerly illegal time
subtraction. Todo has no observations, so its acceptance establishes compilation
only.

[D232](https://github.com/robertguss/mo/commit/c1e660055293d2a9b7976d0364fe44abbf9ecbef)
added the `Json` library module and reserved its name in section 10.3. It also
put the directory-entry type `Entry` in scope in section 10.5. Neither rule was
in the reference the graders read. Both JSON programs declare `module Json`.
Both KV programs declare `struct Entry`, at Claude line 50 and GPT line 6.
Today's four rejections are correct against today's reference. These are real
incompatibilities with later language rules, not mistakes by the Phase 1 agents
or graders. No compatibility exception is added to the checker.

Scratch copies rename only `module Json` to `JsonProbe`, with matching filenames,
or every `Entry` identifier to `StoreEntry`. After those changes, all four check.
Claude's JSON and KV pass 19 and 49 observations. Neither GPT program has any.

### C1. The collection lost the original module path

[The prompt](prompt.md) requested `answers/NAME.mo`. GPT's job-queue and
rate-limit declare `Answers.JobQueue` and `Answers.RateLimit`. These match
`answers/job-queue.mo` and `answers/rate-limit.mo` under reference section 2.1.
Grader B explicitly checked those paths. Collection placed each file directly
under `gpt-6.1-sol/`, so the recorded location no longer matches its declaration.

Copying the bytes unchanged into a scratch `answers/` directory removes both
E0035 diagnostics. This is a collection-layout error, not a program error,
grader mistake, or checker bug. The originals stay in their recorded locations.
The restored job-queue checks on the baseline. Rate-limit next reaches L1.

### P1. Claude job-queue really uses an invalid pattern

Line 64 declares `BadLength(length: Duration)`, but line 141 matches
`BadLength(_)`. Reference section 7.3 requires `BadLength(length: _)` for named
fields. E0067 explains the error and offers exactly that fix. Graders B and C
were right. Grader A missed it. Today's rejection agrees with the settled close
verdict.

Applying only that repair in scratch next reaches L2 on the baseline. With this
PR's lowering fix, the repaired copy checks and passes all 34 observations.

### P2. GPT rate-limit expects the wrong wait after a capped refill

After restoring the path and fixing L1, plain `mo test` reports 12 passed,
one failed, zero stopped. The failure is line 190 in test 4:

```text
left: Refused(wait_microseconds: 333334)
right: Refused(wait_microseconds: 333333)
```

The bucket's capacity is one token. At 333334 microseconds, refill at three
tokens per second exceeds one token, so line 123 caps the credit at one token.
The accepted request consumes that entire token. A request at the same timestamp
therefore waits ceil(1000000 / 3) = 333334 microseconds. The assertion expects
333333, as though the excess credit survived the cap. The program's implementation
is correct for this case. Its test expectation is wrong.

Grader A already identified this error under rate-limit, but kept a pass verdict
because the code compiled and implemented the task. Grader B's claim that all
eight tests' waits were checked by hand is wrong. The aggregate result did not
retain A's caveat. This report records the error without changing the frozen test
or retroactively replacing the recorded verdict. No runtime change is warranted.

### L1 and L2. Small source-to-Core dispatch gaps fixed here

L1 is GPT rate-limit line 123, calling `BigInt.min`. The checker accepts the
method, but `src/lower/expressions.c` lacked mappings for `BigInt.abs`, `min`,
`max`, `pow`, `floor_div`, `floor_mod`, and `to_int`. All seven already have Core
operations, code-generation routes, and VM primitives. Adding the missing
dispatch table fixes this family without changing any shared contract.

L2 is Claude job-queue line 571, `Random.seeded(2026)`, followed by `Random.int`
in the random test. Both had checker, Core, code-generation, and host support,
but neither was in the lowerer's native IO table. Two dispatch entries fix them.

The new `bigint-natives` fixture first failed with E1150 at `BigInt.min`.
The `random-natives` fixture first failed with E1150 at `Random.int`, and the
scratch job-queue reproduced the missing `Random.seeded` route. After the fixes,
both fixtures pass. BigInt checks include numbers larger than Int, both operand
orders for min/max, signed floor division, conversion boundaries, module calls,
dot calls, and function values. Random checks include stream aliasing,
independent equal seeds, and fixed draws derived from the published vectors in
`formal/MoCore/Builtins.lean`, using the mapping in `formal/TRANSLATION.md`.

### L3. Hidden builder dependencies, fixed by MO-127

At MO-121, the unchanged GPT job-queue copied to `answers/job-queue.mo` passed
`check`, but plain `test` refused `acknowledge.promise.1` at line 184 with E1400.
Selecting `fail.promise.1` at line 209 gave the same refusal. Selecting `submit`,
`expire_leases`, or `lease` individually passed each promise.

Both refused subjects take the exposed opaque `Lease`. Its only exposed builder
is `lease(queue: Queue, now: Time, duration: Duration)`, returning it inside
`(Queue, Result(Option(Lease), QueueError))`. `Queue` is also opaque but can start
through exposed `empty()`. Reference section 12.2 permits building hidden types
through bounded calls to their module's exposed functions. `src/tools/promises.c`'s
`startable` rejected opaque builder inputs, so the `Queue` dependency made `Lease`
unstartable. This was a toolchain limitation, not a mistake in the recorded
program.

MO-127 ([#195](https://github.com/robertguss/mo/pull/195)) lets a builder take
hidden types of lower levels, up to three deep, built through their own exposed
functions. On main at `fc53746`, the restored copy passes plain `test` with five
passed, zero failed, and zero stopped, and each of the five `--only` selectors
passes its promise. On [`a127a55`](https://github.com/robertguss/mo/commit/a127a55),
the commit before MO-127, the same copy still stops with E1400 at line 184, so
MO-127 is the change that fixed it.

The Phase 2 item is now traced and every toolchain gap it found is fixed. Both
GPT answers under `Answers.` still stop with E0035 at their recorded paths
because of C1. The restored GPT rate-limit still fails one assertion because of
P2. Those are faults in the collection and in the program, and the originals
stay frozen. D228, D232, and C1
also show why not every difference fits the original item's proposed choice
between a grader mistake and a toolchain fix.

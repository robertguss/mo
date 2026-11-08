# Error-code cases

One folder per case, `eNNNN` or `eNNNN-variant`, checked by `ci/check_errors.py`:

- `main.mo` (and any other modules) and `expected`, the exact standard error of
  `mo check tests/errors/<case>/main.mo`, which exits 1. Every diagnostic in it
  must carry the folder's code.
- A case with a `generated` file names a generator in `ci/check_errors.py` that
  writes a source too big to commit; the expected text then shows `main.mo`.
- A case may add `command` (a subcommand other than `check`), `args` and
  `stdout`.
- `unit.tsv` lists the codes no source program can raise, each with the unit
  test that pins it. A reason starting `UNREACHABLE:` instead points to a
  reachability explanation and disposition. The driver reports these
  separately, never as tested codes. `UNTESTED:` entries fail the gate.

Every code in `src/base/codes.h` must be in exactly one place. Nothing
regenerates `expected`: edit it by hand, and treat a wrong-looking message as a
bug to fix.

## E1106 is a defensive guard, not a source-reachable diagnostic

`infer` in `src/check/bodies.c` reports E1106 at 4,104 simultaneously active
inference frames. `MO_LIMIT_SOURCE_INFERENCE_DEPTH` deliberately allows
16 frames per parser nesting level plus eight outer wrappers. The parser
allows only 256 levels, including the function declaration.

The bound follows the syntax that `src/syntax/parser.c` produces:

- `expr` has seven binary precedence classes, two prefix classes and a
  postfix expression. Descending through that ladder retains at most ten
  expression frames before reaching a primary expression.
- Repeated binary operators, prefixes and postfix steps form flat child
  arrays. Inference walks those arrays in loops, not nested `infer` calls.
- Restarting the ladder requires a group, collection, call argument, lambda,
  conditional, case, loop or interpolation. Each enters a parser nesting
  level. Statement sequences and their statement or binding wrappers add
  fewer than the six remaining frames per level. Sibling statements, arms
  and arguments do not retain one another's inference frames.

`syntax_depth` in `tests/test_check_bodies.c` checks a full precedence ladder
inside 255 nested call arguments through the public inference API. It passes
without E1106. One more nested call exceeds the parser bound and raises
E1006 before inference. The Core path limit E1153 cannot explain this
unreachability: lowering runs after inference.

Retain E1106 and its guard as protection against a future parser or checker
regression. Do not lower the inference limit or fabricate a syntax tree just
to test its message. Revisit this exemption when parser nesting, precedence
or the inference traversal changes. E1053 and E1057 are different: allocation
failure and corrupted compiler output are real failure modes, exercised by
`tests/test_compiler_faults.c` without changing production limits or hooks.

## Live upgrade diagnostics

Live upgrades produce E1414 (upgrade credential), E1415 (busy slot), E1416 (stale
base) and E1418 (malformed upload) through the MCP upgrade tools;
`tests/test_protocol_server.c` pins each. E1431 (ambiguous retained program) comes
from `mo replay --failure ID --data DIR` without a source file; the store test pins
the ambiguity it reports. Existing query codes E1410–E1413 retain their meanings;
the upgrade range starts at E1414. A recorded-program precheck that finds a live
owner emits E1415. `mo serve` retains its existing store-lock message and exit
status 75.

E1428 (live boundary) has no deterministic case. The service refuses a commit with
it when the loop has stopped, but `mo serve` exits as soon as its supervisor stops
the service, so no commit can arrive. The other path is a request that began before
the previous upgrade and still holds that program's catalog when the commit deadline
passes (at most two programs are kept); the request deadlines make that a timing
window, not something a test can hold open without fault injection. Revisit this
exemption if the service can keep serving after a stop.

E1427 is the defensive runtime invalid-timer-interval guard. Source timers must
use positive duration literals, so zero and negative intervals are rejected by
E0103 before upgrade compilation succeeds (reported as E1419 by admission).
The source refusal is covered by the upgrade driver; it is not a runtime E1427
message test. Revisit this exemption if computed timer intervals become legal.

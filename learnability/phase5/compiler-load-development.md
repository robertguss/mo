# Production compiler load controls

Development only; no final collection or worst-case bound is claimed. Before
execution the chosen families were 64 calls of one generic identity function at
distinct nested `List` types, and a binary search of parenthesized expressions
between zero and 257 pairs. `MO_LIMIT_NESTING_DEPTH` is 256. Each candidate is a
service with a reachable read-only tool; its state schema stays Int.

Run from the repository root with a fresh output directory:

```
python3 learnability/phase5/compiler_load.py MO OUTPUT
```

The command uses `mo check CANDIDATE --upgrade-from OLD --json`. This is the
production bundle path: upgrade checking calls `compile_image` for old and
candidate bundles; `compile_image` launches the same executable's compiler child
and `compile_end` loads/verifies its image before `compare_programs` accepts it.
Accepted reports must contain candidate identity and equal, nonempty old/new
schemas. This is more than standalone source checking. The compiler child has
1 GiB address-space, 10-second CPU and 15-second wall limits; those limits are
unchanged. Source bundling/parsing can refuse an invalid input before child launch.
The driver has a 60-second outer command timeout and retains every generated input,
ordinary output, error output, status, command, source hash and elapsed duration.

The successful source-identity-pinned development invocation used
`/tmp/mo-eval-build-aanyuygb/build/gcc/debug/mo`, SHA256
`517da0610ccc341a69dedd202cd8f5439cab695827d8ad60cf2312f8e61703b9`.
The first successful characterization is in `build/compiler-load-development-3`.
After tightening assertions to reject unrelated refusal reasons, the final rerun
is in `build/compiler-load-development-4` (summary `results.json`, all attempts in
`attempts.json`, generated source and raw outputs in each named subdirectory).

- 64 distinct generic-instantiation inputs accepted: 0.362811 seconds.
- 255 nested parentheses accepted: 0.303572 seconds.
- 256 nested parentheses refused E1419 with `code is nested too deeply`:
  0.027312 seconds. Intermediate binary-search attempts remain retained.

The maximum is for this exact parenthesized-expression family under the current
parser, not all syntactic contexts, type depths, recursive instantiations or state
schemas. The source constructs 64 distinct instantiation types; the report does
not expose an emitted-function count. Timings include old and candidate child
compilation plus bundling/loading, not just compiler CPU. These finite cases do
not establish worst-case host stalls, OS confinement or a portable latency bound.
No live activation or migration pause is measured here.

Failures kept: `build/compiler-load-development-1` refused before compilation
because `old.mo` declared Main. The old source now uses `old/main.mo`.
`build/compiler-load-development-2` refused E0101 because the Int-state service's
tool returned Int directly; the fixture now returns Bool. Neither refusal is
counted as nesting or generic evidence. The later nesting oracle requires its
specific refusal message, preventing this class of false positive.

`python3 -m py_compile learnability/phase5/compiler_load.py` passed. No source,
compiler limits or production binary were modified by the controls. No process
remains running. Final collection must use the parent's frozen verified inputs;
these development outputs do not close that gate.

## Review correction: failed-attempt retention

The initial driver wrote its attempt ledger only after process completion and
JSON parsing. A timeout or malformed JSON could omit the failed attempt and its
partial output. The corrected driver writes failure metadata and elapsed time
in a finally block, retaining partial stdout/stderr on timeout.
`test_capture_failures.py` checks timeout output retention for both compiler and
measurement commands and malformed compiler-output retention. The combined
instrument tests passed 11 cases with real snapshot controls enabled.

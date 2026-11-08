# Production activation timing probe

Build with `-DMO_UPGRADE_PAUSE_TIMING` in the compiler include/options flags.
It is absent normally; no runtime option or public report field is added.
For example (separate output trees avoid mixing ordinary and instrumented objects):

```
make -j8 CC=gcc BUILD=build/pause-gcc INCLUDES='-Isrc -Ibuild/generated -Ivendor/sqlite -DMO_UPGRADE_PAUSE_TIMING'
```

stderr receives JSON lines tagged `mo_pause: 1`, with a process-local activation
`attempt`, bounded `stage` and `outcome` strings, and `elapsed_ns`. Correlate by
process and attempt in a single-service, serial driver. Migration records outside
production activation have attempt zero and are not boundary measurements.
This probe assumes the existing serial activation path, not concurrent callers.
No state, source, credential, migration name or service identifier is printed.

`boundary_to_commit` starts on entry to production `activate` and ends immediately
after the store call returns, before cleanup or diagnostic output. `store_activate`
uses the same end timestamp. Outcomes distinguish committed, refused and unknown
(the store's unsafe result). Earlier refusals end the boundary at refusal;
those records do not represent a durable commit. An absent stage was not reached,
never a zero-duration success; a null duration means the clock failed or regressed.
A failed stderr write can lose records; output is deliberately best effort and
cannot change upgrade acceptance. Incomplete records are not usable evidence.

Stages cover cached-state lookup, total live admission, protocol catalog,
swap preparation (including new-loop restoration and queue preparation),
activation-report preparation and durable store activation. Cached state reports
`cache_hit_no_encoding` or `cache_miss_no_encoding`: this API retrieves the existing
buffer and does no encoding. No retained-snapshot encode timing is invented.

Within admission, explicit migration records time each attempted old-value decode,
the migration call, output encoding, validation decode and timer validation.
Without a migration function, `identity_decode` reports `ok_no_migration`; there
is no migration-call record. Explicit identity functions still use `migration_call`.
Timer validation with no timers is an empty loop; `not_run` means earlier failure.
The public report retains its existing VM/native work, heap peak and encoded bytes.

Clock reads and synchronous stderr writes perturb execution. In particular,
boundary and admission totals include nested record emission; durations must not
be summed, treated as uninstrumented latency, or used as deadline guarantees.
Redirect stderr to a local file and disclose this overhead in later measurements.
This change is instrumentation only, not a measurement driver or final collection.
The nightly repair is not merged; no final collection is authorized here.

Verification scope: compile ordinary and instrumented debug binaries with GCC and
Clang, and run the existing upgrade-store unit test. No timing-specific test or
workload is added because this bounded task excludes drivers and owns only the
probe sources and this note. Compilation verifies both macro branches; it does
not establish production latency or full activation-path coverage.

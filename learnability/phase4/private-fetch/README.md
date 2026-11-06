# Private fetch development checks

These are implementation regressions and native-client workflow smoke, not scored diagnostic
observations or evidence of agent effectiveness. The pilot remains 30 attempts, 29 completed
grades, interrupted pilot7, four uncalibrated variants and no independent baseline.

Run the socket/filesystem regressions with `python3 ci/check_fetch.py MO`. Add `--expiry` for
the real 60-second idle-expiry case. Default cases cover private nominal state, full-page
coverage, exact independently specified Text state while commits occur, fresh retry generation,
replacement, final-page release, cancellation after a valid page, corrupt continuation/final
digest, proxy configuration, wrong authentication, malformed responses and destination safety.
The compiled peer/deadline tests cover refused resolver/actual-peer results before credential
transmission, cancellation at temporary creation and interruptible filesystem stalls.

`python3 learnability/phase4/private-fetch/client_probe.py MO OUT` runs the existing recording
forwarder and Codex 0.166.0 using `gpt-6.1-sol` at `high`, explicit tool approval and shell network
access. The prompt gives the host-local command; it verifies discoverability and the supported
workflow, not unaided diagnosis. The development run discovered/routed `mo-overview`/`mo-export`,
then observed fetch exit 0, mode 0600 and locally read `MOEX` magic. An earlier run with duplicate
model flags failed before starting; a later shell-network-sandbox refusal is retained alongside
the successful run under `build/u9-native-smoke*`. No comparative effectiveness claim follows.

Run full process/pin/scratch sizing with:

```sh
python3 learnability/phase4/private-fetch/sizing.py MO RESULT.json
python3 learnability/phase4/private-fetch/sizing.py MO MAXIMUM.json 16767505
```

The second fixture targets the 16 MiB saved encoding including the private record's other fields
and schema. Both retain the source's private nominal state, scalar author tool and timer. The
harness takes one active memory observation and records each process's kernel RSS high-water
mark, pin bytes, heap/scratch observations, artifact identities/digests and independent state
checks. Nominal structs lower to a single-constructor Choice: their payload has a four-byte case
index, then a four-byte field count before fields (saved-state specification and `lower_data`).
The initial sizing oracle omitted this header and failed after successful fetches; its first
correction still omitted the case index. The corrected oracle was independently checked on the
14-page private-state fixture before repeating full sizing. Failed oracle runs are retained in
`build/u9-sizing-{lookahead,complete,maximum}-result.txt`.

Earlier serial transport attempts reached 600.48, 600.05 and 600.10 seconds without publishing,
including a low-traffic control. Their logs/results remain in `/tmp/mo-u9-size-*`. The bounded
one-page lookahead overlaps server rendering and client parsing on the measured 8-vCPU Linux
host; it never skips ordered verification. The ten-minute deadline remains unchanged. Final
sizing passed: the complete 16 MiB saved state fetched across 2,049 pages in 532.07 seconds,
with service/fetch RSS peaks of 135,928/58,972 KiB, a 16,777,437-byte pin, 88,222-byte reported
diagnostic scratch peak and zero pinned bytes after completion. The mode-0600 artifact remained
local under `build/`; only nonconfidential measurement metadata is retained in
[results/sizing-maximum.json](results/sizing-maximum.json).

GCC, Clang, ASan/UBSan, all-build drivers, static checks, TLC, reader/CLI fuzz and scoped CBMC
proof/reach checks passed. The final all-build fetch rerun additionally pins program/build/schema
identities and exact state during commits, and exercises IPv4, localhost and IPv6 loopback.
See [results/verification.md](results/verification.md) for commands, review closure, retained
failures and limits. Native-client observations remain prompted workflow smoke only.

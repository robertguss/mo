# Streaming resource development

Predeclared synthetic measurement: 512 repeated regular-file read records, each
with 262144 payload characters, plus exec and terminal records; maximum decoded
input 135 MiB. No random seed (deterministic repetition). One compressed artifact
under build/resource-stream-development (0700 directory, 0600 file), no decoded
disk copy. Measure compression plus parse and integrity passes in a fresh Python
process. This is not a model session or one-hour client qualification.

Initial disk availability: 399806464 bytes. Existing private artifacts are untouched.
The checkout has no codebase-memory index; source reads supplied coverage.

Measured 514 records (512 reads): 134238778 decoded bytes, 144853 gzip bytes.
Original SHA256: 968af5d9cab1bb550d1d0e105222fd832b56818de400e23a0347b1c140e23df2.
Stored SHA256: d3ff69a0aaf4847c850359293fe02dbcaf15d0d29cd2a3a7d54bb5a3033e6498.
Decompression digest matched; parser output matched the one-record control with
its read count adjusted to 512; integrity output matched exactly.
Compression plus validation took 2.611119 seconds. That process reported
598192 KiB ru_maxrss: this unexpectedly high observation is retained, not a
bounded-memory success claim. A separate fresh replay process reported 20876 KiB
both ru_maxrss and Linux VmHWM, 19276 KiB final RSS, 1.994483 seconds for parsing
and integrity. These are different workloads; their times are not speedup ratios.
Final measured free disk: 398557184 bytes. Private measurement JSONs retain results.

Validation: two storage tests passed (0.028s); 45 origin tests passed (0.421s).
Tests cover byte identity, stored mode, repeat passes, paired birth ordering,
corrupted/truncated gzip, missing final newline and oversized record rejection.
The string parser adapter remains compatible. The CLI now uses streaming input.

Incomplete integration: TraceRun still writes plain traces and reads them whole;
Capture.consume, Bundle, protected_regrade and protected() still materialize
whole files. compress_stream is a tested component, not wired to live capture.
No capture format changed, and no isolated curl/urllib/push controls were run.
Birth replacement metadata grows with fork count; HTTP connection buffers,
pipe/stdout events and receipt collections still grow with their input. The 8 MiB
record bound explicitly rejects larger records; it is not a coverage assertion.
No full-hour readiness, provenance, or collection qualification is established.
Original 0/4 remains unchanged; model provenance remains false and collection disabled.

No model/scored runs, C builds, Git/PR actions, old artifact mutations, firewall
changes or credential changes occurred. Final credential-inventory scan remains
pending: inventory locations were not established in this implementation turn.

Integration work in progress: production Capture, Bundle, ReplayReceipt and
reconstruction now stream trace reads/copies; protected() has a pin-only mode.
Snapshot hashes bind stored bytes (plain for legacy fixtures, gzip for new
captures). TraceRun writes gzip during capture through a private FIFO and returns
decoded byte count/SHA256 separately. No post-seal conversion is performed.
Focused validation passed 45 origin, 31 bundle, 46 protected-regrade tests;
subsequently 7 observer tests passed including trace read_bytes prohibition and
launch-failure cleanup. Broad discovery was interrupted and is not a pass.
The requested isolated controls, actual DEVELOPMENT collector/regrade, production
RSS measurement and final private-inventory scan remain outstanding. This work
is incomplete and must not be used to qualify full-hour resource behavior.
The earlier 598192 KiB failure and all unbounded collections above still apply.

## Parent production verification

Declared one isolated live-control run (curl, urllib, migrating push plus unrelated curl): all passed with 1/2/7 exact observations, six verified push requests, accepted ordinal 1. Evidence /root/mo-origin-live-musaclo4; log build/stream-origin-live.txt. New captures use gzip, stored-byte pins, and a separate decoded byte count/SHA256.

Declared one DEVELOPMENT success collector and two replays: one test passed in 296.809 seconds (build/stream-real-replay.txt), evidence /home/exedev/phase5-test-private-6uner697. Collector 146.491 seconds; replays 73.508 and 76.585 seconds. Both returned six non-model dimensions true, model and overall false, protected DEVELOPMENT classification and all retained pins unchanged. Trace decoded bytes 145543998; stored bytes 18773477; retained bundle 27239558 bytes. Parent independently decompressed and checked the recorded decoded SHA256. This is lossless storage evidence, not a full-hour capacity claim.

The combined process again reported 598192 KiB peak RSS. That matches the earlier combined synthetic observation numerically; the cause is unresolved. No bounded-memory success is claimed from this result. Future measurement must include Linux VmHWM and a pre-work baseline to distinguish inherited/process accounting from actual workload peak; the separate 20876 KiB component replay remains a different workload. Collections of HTTP buffers, stdout, pipe events and process metadata still need bounds review.

Parent focused verification: 174 tests passed in 3.803 seconds, build/stream-parent-focused.txt; whitespace check passed. Final cleanup verified both isolated accounts empty, exact OUTPUT ACCEPT, home0750 and both fake credentials matching the retained snapshot. Scanner passed 17 publication-eligible files against service/provider inventories; raw compressed traces remain private and excluded. All checks ended. Full five-case collector, lifecycle review, full-hour resource qualification and model provenance remain pending.

## Bounded real-bundle accounting investigation

Predeclared before execution: exactly one fresh no-work baseline, one streaming
parse + integrity + decoded hash process, one protected regrade, and one 64 MiB
allocation/fork/exec probe with one no-work child. All completed, no retries.
No collector, installation, account change, C build, model, Git or PR action.
Evidence directory: build/resource-real-investigation (0700; JSON files 0600).
Only resource_probe_development.py and this note were added/edited in source.
The checkout has no graph index (coverage tool confirmed); direct source reads
covered trace_storage, observer, identity, bundle and protected replay.

Exact commands (cwd /home/exedev/code/mo-m6), after creating the private directory:
```sh
python3 learnability/phase5/evaluation/resource_probe_development.py baseline build/resource-real-investigation/baseline.json
python3 learnability/phase5/evaluation/resource_probe_development.py probe build/resource-real-investigation/probe.json
python3 learnability/phase5/evaluation/resource_probe_development.py stream build/resource-real-investigation/stream.json
python3 learnability/phase5/evaluation/resource_probe_development.py regrade /home/exedev/code/mo-m6/build/resource-real-investigation/regrade.json
```

All memory values below are KiB. Initial/final fields are VmRSS, VmHWM,
and resource RUSAGE_SELF ru_maxrss; periodic sampling interval is 10 ms.

- baseline.json: initial {'VmHWM': 14712, 'VmRSS': 14712, 'ru_maxrss': 14576}; final {'VmHWM': 14940, 'VmRSS': 14940, 'ru_maxrss': 14832}; sampled peaks {'self': 14812, 'children_sum': 0, 'child_individual': 0}; elapsed 0.000586 seconds. SHA256 2ec16d27f0f6c65f4a1e4b7724ec550e087621bea9d528bd40db63acbcb4321a.
- probe.json: initial {'VmHWM': 14740, 'VmRSS': 14740, 'ru_maxrss': 14604}; final {'VmHWM': 80712, 'VmRSS': 15524, 'ru_maxrss': 80712}; sampled peaks {'self': 80940, 'children_sum': 74916, 'child_individual': 74916}; elapsed 0.056763 seconds. SHA256 24e9bade3e708ca0ff4df64d9e02cada50870ce45be849a08d7dae4be7bf6a95.
- probe.json.child.json: initial {'VmHWM': 14708, 'VmRSS': 14708, 'ru_maxrss': 74800}; final {'VmHWM': 15004, 'VmRSS': 15004, 'ru_maxrss': 74800}; sampled peaks {'self': 14808, 'children_sum': 0, 'child_individual': 0}; elapsed 0.000550 seconds. SHA256 6f8879c112a0846b58f90b8ddebe24b6b9ca4c9d6f5cc6c2b536fdb9cab621a4.
- stream.json: initial {'VmHWM': 14740, 'VmRSS': 14740, 'ru_maxrss': 14604}; final {'VmHWM': 36856, 'VmRSS': 36856, 'ru_maxrss': 36540}; sampled peaks {'self': 36796, 'children_sum': 0, 'child_individual': 0}; elapsed 2.572607 seconds. SHA256 f05c1521d21a9b1fe6fce68e7e2181de979131962ed187ac0cc3b316258ede97.
- regrade.json: initial {'VmHWM': 14724, 'VmRSS': 14724, 'ru_maxrss': 598192}; final {'VmHWM': 40880, 'VmRSS': 40880, 'ru_maxrss': 598192}; sampled peaks {'self': 40884, 'children_sum': 67104, 'child_individual': 67104}; elapsed 75.861530 seconds. SHA256 9ee3673c354dd13a8f07dd3f91e776ba0ce9eacb5fc0c964a17ec6d05144edea.

The real regrade already had 598192 KiB ru_maxrss BEFORE imports or workload,
while initial VmHWM/VmRSS were 14724 KiB. It finished with that same ru_maxrss
and VmHWM 40880 KiB. Thus 598192 is a retained pre-work accounting floor in
this run, not a measured regrade resident-memory peak. The controlled probe
independently reproduced the mechanism: after allocation and fork/exec, the
fresh child had ru_maxrss 74800 KiB but initial VmHWM 14708 KiB, final 15004 KiB.
Linux preserves resource high-water accounting across exec while the new memory
image has its own /proc high-water measurement. This supports an accounting
explanation for the historical identical values; those old runs lack initial
/proc measurements, so their actual peaks remain unknown. The probe emitted
Python's multithreaded-fork warning; its child immediately execed, exited zero,
and was waited for. No process remained running.

Regrade: six non-model dimensions true, model_provenance false, correct false,
DEVELOPMENT. Trusted binding used the explicit existing session path and isolated
account UID, verified against protected manifest, with exact original collector
pins checked before and after and bundle verification repeated. Workspace was
separate and private; no retained artifact changed or was repinned/hardlinked.
Stream: seven requests/seven responses, process completeness true. Decoded bytes
145543998, stored trace 18773477, bundle 27239558. Decoded SHA256
 eb2fab3a73ea05bb00a888471e7b80533d9fac4cc8865115eba5d81c4ec15c74.

Precise limits: Python regrade kernel VmHWM 40880 KiB; sampled self maximum
40884 KiB (slightly different Linux accounting observations); sampled descendant
sum and individual maxima both 67104 KiB. Children were sampled as a process
tree, not classified by executable, so this is not a separately attributed Mo
kernel high-water guarantee. Sampling can miss short-lived children/peaks and
reads are not atomic. No workload ratios, full-hour capacity or readiness claim.
The collector itself was NOT rerun or measured here.

Remaining growing state: origin_trace retains tx/rx payloads, connection event
lists, stdout/pipe/exec events, process/mapping dictionaries and receipts;
order_births retains replacements/removals and integrity retains process/error
metadata. Snapshot JSON, proxy rows, stdout, ELF/mapping reads still materialize
non-trace files. The 8 MiB record limit and bundle stored-byte/file caps do not
bound these aggregate structures. Proposed next evidence, not executed: three
fresh deterministic offline fixtures at 1/2/4 MiB aggregate connection payload,
then three at 100/1000/10000 process/event records, each capped at 32 MiB decoded
input and 30 seconds, with identical /proc and child sampling. Establish growth
by structure before choosing caps/spooling. No automatic collectors/models.

Full five-case collector and lifecycle review remain pending; the accounting
ambiguity is resolved for this measured regrade, but collector peak and long-run
capacity remain unmeasured. Model proof remains unqualified, scoring disabled,
original 0/4 unchanged. No production change justified or made. Free disk after
measurement was 330305536 bytes; compressed test executables left untouched.

Parent storage maintenance after the full collector: consolidated 75 previously gzip-compressed generated sanitizer test executables into build/sanitizer-test-executables.tar.xz, verifying every original SHA-256 and full member inventory before deleting the redundant gzip copies. Recovered 691824601 bytes; receipt build/sanitizer-test-archive-receipt.json includes sizes/hashes/modes. Two slower compression attempts were stopped with originals preserved, and a redundant end-of-stream verifier call failed before a corrected complete verification. These are build-artifact maintenance failures, not evaluation runs. No source/log/capture evidence changed. GCC debug test executables were restored from their previously verified gzip copies for the required repository check; sanitizer executables remain restorable from the verified tar archive.

## Resumed bounded growth measurement declaration

Before execution: six fresh offline parser processes, connection payloads of
1/2/4 MiB and process/exec records of 100/1000/10000, in that order. Each uses a
reiterable generated trace, at most 32 MiB decoded, and a 30-second timeout.
Payloads are HTTP request bodies in 16 KiB chunks on one connection; process
fixtures use distinct PIDs and exec records. Record initial/final Linux VmRSS
and VmHWM separately from ru_maxrss. These synthetic parser-only workloads do
not qualify the collector or one-hour sessions. No model sessions are launched.

First growth batch: process fixtures completed; all three payload fixtures failed
with truncated HTTP body because the fixture subtracted eight escape bytes
instead of six. Original logs remain in build/resource-growth-resume.
Declare exactly three corrective payload runs (1/2/4 MiB), same bounds; use
the decoder to derive syscall byte lengths. No process fixture rerun.

All six valid fixtures completed within their bounds. Machine-readable results:

```json
[
  {
    "kind": "payload",
    "count": 1,
    "initial": {
      "VmHWM": 16956,
      "VmRSS": 16956
    },
    "final": {
      "VmHWM": 21568,
      "VmRSS": 21568
    },
    "ru_maxrss": 21380,
    "elapsed_seconds": 0.18590809707529843,
    "decoded_bytes": 1055936,
    "requests": 64,
    "exec_events": 0
  },
  {
    "kind": "payload",
    "count": 2,
    "initial": {
      "VmHWM": 16932,
      "VmRSS": 16932
    },
    "final": {
      "VmHWM": 22700,
      "VmRSS": 18516
    },
    "ru_maxrss": 22700,
    "elapsed_seconds": 0.28646352398209274,
    "decoded_bytes": 2111872,
    "requests": 128,
    "exec_events": 0
  },
  {
    "kind": "payload",
    "count": 4,
    "initial": {
      "VmHWM": 16944,
      "VmRSS": 16944
    },
    "final": {
      "VmHWM": 27192,
      "VmRSS": 18708
    },
    "ru_maxrss": 27192,
    "elapsed_seconds": 0.5763226500712335,
    "decoded_bytes": 4223744,
    "requests": 256,
    "exec_events": 0
  },
  {
    "kind": "process",
    "count": 100,
    "initial": {
      "VmHWM": 16900,
      "VmRSS": 16900
    },
    "final": {
      "VmHWM": 18404,
      "VmRSS": 18404
    },
    "ru_maxrss": 18296,
    "elapsed_seconds": 0.006764716003090143,
    "decoded_bytes": 3910,
    "requests": 0,
    "exec_events": 100
  },
  {
    "kind": "process",
    "count": 1000,
    "initial": {
      "VmHWM": 16900,
      "VmRSS": 16900
    },
    "final": {
      "VmHWM": 19336,
      "VmRSS": 19336
    },
    "ru_maxrss": 19256,
    "elapsed_seconds": 0.025620385073125362,
    "decoded_bytes": 39920,
    "requests": 0,
    "exec_events": 1000
  },
  {
    "kind": "process",
    "count": 10000,
    "initial": {
      "VmHWM": 16956,
      "VmRSS": 16956
    },
    "final": {
      "VmHWM": 29488,
      "VmRSS": 28760
    },
    "ru_maxrss": 29488,
    "elapsed_seconds": 0.1906745620071888,
    "decoded_bytes": 408930,
    "requests": 0,
    "exec_events": 10000
  }
]
```

These measurements establish workload-dependent parser memory growth, not an
aggregate memory bound. No collector, replay, child-memory sampling, full-hour
qualification or model serving was exercised. The generator itself is streamed;
parser process/event/HTTP retention remains workload dependent. Further work
requires aggregate limits or spooling and full collector capacity qualification.

Storage maintenance completed: 75 generated GCC test executables were gzip
compressed, each decompressed and SHA-256 checked before its original was
removed. Restoration paths, hashes, modes and mtimes are retained in
build/resume-gcc-compression.json. Recovered 302592000 bytes; final available
space 534081536 bytes. Restore these executables before running GCC checks
(or rebuild). No sealed evidence or unrelated project/cache was changed.
Validation: six saved result counts/bounds checked independently; 26 focused
parser/storage/model-adapter tests passed; credential scanner found zero
matches across 116 eligible files and two inventories; diff check passed.
No model or scored launches, no full-hour readiness claim.

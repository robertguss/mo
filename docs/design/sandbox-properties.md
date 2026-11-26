# Mo Sandbox Properties

What the Mo toolchain guarantees about a program it loads and runs, where each guarantee is enforced, what evidence backs it, and what is explicitly outside scope. Following Wasmtime's security documentation style, every claim is stated as a numbered property with concrete enforcement points, evidence, and gaps.

This document summarizes facts defined by [`bytecode.md`](bytecode.md) (verification rules), [`architecture.md`](architecture.md) (capabilities, execution, and testing), and [`outside-world.md`](outside-world.md) (network, SQLite, and child confinement). Where they disagree, those specifications win.

---

## Threat model

The untrusted input is a **bytecode file** (`.bc`, whether compiled locally or uploaded as a service upgrade) and the **Mo program it encodes**. The runtime must prevent a loaded program from:

1. reading or corrupting memory outside its isolated VM heap;
2. reaching the outside world except through capabilities explicitly granted to it; or
3. exceeding its configured stack, call-depth, heap, or work budgets.

**Trusted computing base (TCB):** The pure-Rust runtime crates—`mo-def`, `mo-bytecode`, `mo-heap`, `mo-prim`, `mo-host`, `mo-vm`, and `mo-service`—plus the Rust standard library and their audited third-party dependencies (`rustls`/`ring`, RustCrypto `sha2`/`hmac`/`aes-gcm`/`scrypt`/`subtle`, `rustix`, `landlock`, `seccompiler`, and bundled SQLite via `rusqlite`). Every crate in `crates/` enforces `#![forbid(unsafe_code)]`, with zero OpenSSL and zero system `libsqlite3` dynamic linkage (audited in `ci/check_rust_frontend.py --audit-only`).

The compiler front end (`mo-syntax`, `mo-check`, `mo-lower`, `mo-core`, `mo-codegen`) is **not** trusted by the VM for memory or capability safety: `mo-vm` never sees source or Core, and `mo-bytecode` verifies every loaded image regardless of what produced it.

**Out of scope:** A compromised host operating system, concurrent external processes mutating directories during path lookup (P8), hardware faults, and timing or microarchitectural side channels.

---

## Evidence kinds

| Evidence | What it is | How it runs |
| --- | --- | --- |
| **Safe Rust (`#![forbid(unsafe_code)]`)** | Compiler-enforced memory safety across all 17 workspace crates; linkage audit forbids OpenSSL and system `libsqlite3`. | Every `cargo build` and `bash ci/check.sh rust` |
| **Load-time verifier** | `crates/mo-bytecode/src/{read,validate,verify}.rs`: refuses malformed, ill-typed, ownership-invalid, or spec-leaking bytecode before execution. | Every image load |
| **Workspace tests** | Rust unit and integration tests across `crates/*/src/*` and `crates/*/tests/*`, including 48-directory `fuzz/corpus/` replay (`corpus_replay.rs`), Linux Landlock/Seccomp/rlimit child confinement tests (`confinement_tests.rs`), and VM/verifier property and fuzz-smoke tests. | `bash ci/check.sh rust` |
| **Black-box drivers** | End-to-end tests of CLI, host confinement, live HTTP/TLS, SQLite, Command, services, upgrades, fetch, offline replay, and SIGKILL recovery. | `bash ci/check.sh drivers` |
| **Lean 4 formal proofs** | Mechanized type-safety, saved-value round-trip, and replay-verdict theorems (`formal/`), plus Rust-vs-Lean Core differential tests. | `bash ci/check.sh lean` |
| **TLA+ models** | Finite state-machine verification of export generations, failure capture, and live upgrade swaps with negative controls (`models/`). | `bash ci/check.sh models` |

---

## Properties

### P1. A loaded file is well formed

*Every accepted bytecode image has every section count, table index, string offset, and constant within declared bounds (`crates/mo-def/defs/limits.h`), valid UTF-8 strings, and well-formed types and layouts.*

- **Enforced by:** `crates/mo-bytecode/src/read.rs` and `validate.rs` (`bytecode.md` §9). Refusals report a specific error and byte offset.
- **Evidence:** `crates/mo-bytecode` unit tests, binary/text round-trip tests, and corpus fuzz tests (`fuzz/corpus/`).
- **Not covered:** Bounded corpus fuzzing does not formally prove reader completeness.

### P2. Every operand has the static type its instruction requires

*No register is read at a mismatched type, and no field, choice case, layout, primitive, or host operation index is out of range.*

- **Enforced by:** `crates/mo-bytecode/src/verify.rs` checking every instruction against `crates/mo-def/defs/{opcodes.def,prims.def,hostops.def}` and runtime type layouts.
- **Evidence:** `crates/mo-bytecode` verifier tests, `crates/mo-vm` tests (including `fuzz_tests.rs`), and Lean 4 Core type-safety proofs (`formal/MoCore/Theorems.lean`) paired with differential runs (`ci/check.sh lean`).
- **Not covered:** Type-checking rules in `mo-bytecode` are tested and fuzzed rather than mechanically proved in Lean against the bytecode semantics.

### P3. Memory use is safe and reference counts balance

*A verified program frees every owned reference exactly once, never reads an undefined or moved register, never double-frees or leaks at return, and cannot corrupt host memory.*

- **Enforced by:**
  1. `#![forbid(unsafe_code)]` across the entire `mo-heap` / `mo-vm` / `mo-prim` / `mo-host` runtime—preventing raw-pointer UAF, buffer overflows, and memory corruption by construction.
  2. Ownership dataflow verification in `crates/mo-bytecode/src/verify.rs` (`bytecode.md` §§4.2, 9): tracks `Undefined`, `Value`, `Owned`, `Borrowed`, and reuse-token states across every block join and requires zero owned registers on `RET` and tail calls (`STOP`/`FAIL` discard the process heap whole).
- **Evidence:** `crates/mo-bytecode` ownership verification tests, `crates/mo-heap` and `crates/mo-vm` Perceus reuse and leak-check tests (`fuzz_tests.rs`), and `ci/check.sh drivers`.
- **Not covered:** Logic bugs in reference-count accounting can still manifest as safe runtime panics or budget errors rather than memory unsafety.

### P4. Control flow stays inside the function

*Every `JUMP`, `BRANCH`, and `SWITCH` targets the start of an instruction within the same function; no path falls off the end; unreachable instructions are rejected.*

- **Enforced by:** `crates/mo-bytecode/src/{validate.rs,verify.rs}` (`bytecode.md` §9).
- **Evidence:** `crates/mo-bytecode` CFG verification tests and `crates/mo-vm` fuzz tests.
- **Not covered:** Infinite loops are permitted by the verifier (see P9 for work bounds).

### P5. Verification is bounded in time and memory

*The verifier rejects pathological inputs that exceed verification work or scratch-memory ceilings rather than exhausting host CPU or RAM.*

- **Enforced by:** Named ceilings in `crates/mo-def/defs/limits.h` (`MO_LIMIT_FUNCTION_REGISTER_BLOCKS`, `MO_LIMIT_VERIFY_WORK_PER_WORD`, `MO_LIMIT_VERIFY_BYTES_PER_WORD`), explicit worklists (no unbounded recursion), and linear-time ability and initialization summaries in `crates/mo-bytecode/src/verify.rs`.
- **Evidence:** Verifier scaling regression tests in `crates/mo-bytecode` and `fuzz/corpus/`.
- **Not covered:** Bounds are checked empirically and by unit tests rather than formally proved.

### P6. Globals are initialized once in dependency order

*Every global has exactly one initializer function, no initializer reads an uninitialized global through direct calls, and non-init code never calls an `init` function.*

- **Enforced by:** Global rank summaries in `crates/mo-bytecode/src/verify.rs` and single-assignment initialization in `crates/mo-vm`.
- **Evidence:** `crates/mo-bytecode` and `crates/mo-vm` tests.
- **Not covered:** Indirect reads through higher-order closures are not statically ranked by the verifier; `mo-vm` retains a runtime guard that stops cleanly on an uninitialized global read.

### P7. The outside world is reachable only through explicit capabilities

*A program can perform I/O, networking, database access, randomness, clock reads, or subprocess execution only through capability values passed to it; production code can never invoke spec code or forge fake capabilities.*

- **Enforced by:**
  - All host effects dispatch exclusively through `HOST` instructions (`crates/mo-def/defs/hostops.def`) on capability handles instantiated by `mo-cli`/`mo-service` for `main` or `api` declarations.
  - `crates/mo-bytecode/src/verify.rs` forbids non-spec functions from calling, tail-calling, or capturing spec functions, fake capability constructors, `printed`/`printed_errors`, or `fail`.
  - Narrowing operations (`Files.within`, `ReadFiles.within`, `Files.read_only`, `Network.allow_hosts`, `Command.within`, `Command.allow_programs`) are strictly monotonic; confined `Command` blocks all network sockets by default, while `Command.allow_loopback(net)` requires a `Network` capability whose allowlist permits loopback (`127.0.0.1`, `localhost`, or `::1`), denies `socket(AF_UNIX, ...)` filesystem/abstract Unix sockets, datagram socketpairs, UDP/raw/`AF_NETLINK`/`AF_PACKET` sockets, and TCP connects to ports not listening on loopback at child start (Unix stream socketpairs remain permitted). Supervised processes and commands with loopback enabled fail closed if confinement is unavailable.
  - `Connection`, `Sqlite.Database`, `Files.File`, and `Command.Process` handles can only be minted by `Network`, `Sqlite`, `Files`/`ReadFiles`, and `Command` (and live `Command.Process` handles kill and reap their child tree when dropped); durable `service` blocks receive no capabilities at all.
  - `Sqlite.Database` installs a per-connection SQLite authorizer blocking `ATTACH`, `DETACH`, `VACUUM INTO`, `PRAGMA temp_store*`, and extension loading, keeping temporary storage in memory.
- **Evidence:** `crates/mo-host`, `crates/mo-bytecode`, and `crates/mo-vm` tests; `ci/check_network.py`, `ci/check_golden.py`, `ci/check_programs.py`.
- **Not covered:** SQL statements inside an already-granted `Sqlite.Database` can still call SQLite built-ins such as `datetime('now')` or `random()` without `Clock` or `Random`. Under `Command.allow_loopback(net)`, Linux Landlock `NetPort` rules filter TCP destinations and binds by port number rather than IP address: a port already listening on loopback at child start can be connected on non-loopback interfaces on that same port, and ephemeral `bind(0)` is permitted without IP filtering.

### P8. File, SQLite, and Command capabilities cannot escape their granted directory

*`Files`, `ReadFiles`, `Files.File`, `Sqlite`, and `Command` resolve paths strictly beneath the directory handle they hold, rejecting `..` escapes, absolute paths, and escaping symlinks.*

- **Enforced by:** Lexical path normalization plus kernel-enforced `openat2` with `RESOLVE_BENEATH` on Linux (falling back to component-by-component `openat` with `O_NOFOLLOW` and bounded link/step limits on other Unix hosts) in `crates/mo-host/`. `Sqlite.open` resolves parent directories through the same confined resolver and verifies database and `-journal`/`-wal`/`-shm` sidecars with `O_NOFOLLOW`/`SQLITE_OPEN_NOFOLLOW`.
- **Evidence:** `crates/mo-host` path-confinement and hostile-symlink unit/integration tests and golden tests (`tests/golden/cap-files-within-escape/`, `tests/golden/cap-files-links/`).
- **Not covered:** Another process concurrently renaming an intermediate directory outside Mo during a lookup (directory-object authority, as in Capsicum/WASI/`cap-std`, rather than an atomic filesystem snapshot).

### P9. Stack, call depth, and heap are bounded

*Every VM process enforces `--max-stack` (default 8 MiB), `--max-depth` (default 10,000), `--max-heap` (default 1 GiB), and per-value ceilings (`MO_LIMIT_VALUE_BYTES`). Exceeding any limit stops cleanly with a defect report.*

- **Enforced by:** `mo-vm` stack/depth checks and `mo-heap` `HeapBudget` accounting. Service handlers, `api` requests, queries, and tests additionally enforce instruction budgets (`MO_LIMIT_SERVICE_WORK`, `--max-work`).
- **Evidence:** `crates/mo-heap`, `crates/mo-vm`, `crates/mo-service`, and `ci/check_cli.py` limit tests.
- **Not covered:** Default `mo run` enforces no wall-clock or instruction limit (halting is undecidable; embedders, services, and `mo query` set explicit work budgets).

### P10. Defects stop cleanly with isolated heaps

*Arithmetic overflow, division by zero, `stop`, and budget exhaustion terminate only the failing VM process, render a defect report from readable registers only, and reclaim the process heap whole.*

- **Enforced by:** `crates/mo-vm` stop handling and `mo-bytecode` readability replay; per-request `mo_process` isolation in `mo-service` (`api` workers roll back open SQLite transactions automatically on stop; `service` handlers roll back uncommitted state and capture a bounded failure record).
- **Evidence:** `crates/mo-vm` defect tests, `ci/check_offline.py`, `ci/check_offline_core.py`, and Lean 4 differential checks (`ci/check.sh lean`).
- **Not covered:** Differential tests against Lean exercise pure and fake-capability Core programs, not live multi-threaded socket workloads.

### P11. Restored saved-state and export bytes are schema-validated before execution

*Loading a service snapshot, replaying a recovery log, accepting an actor cross-heap message, or opening an exported `.moex` artifact validates every encoded tag, length, collection distinctness invariant, and type schema before any handler sees the value.*

- **Enforced by:** `crates/mo-heap` saved-value codec (`MOSV`), `crates/mo-service` store/export verification (generation ID, sequence coverage, page checksums, schema digest, trailing SHA-256), and `crates/mo-upgrade` offline checks.
- **Evidence:** Lean 4 proofs of saved-state round-trip and schema refusal (`formal/MoCore/Saved.lean`) and replay-verdict soundness (`formal/MoCore/Verdict.lean`), TLA+ export/capture/swap models (`models/`), `ci/check_export.py`, `ci/check_fetch.py`, `ci/check_offline.py`, and `ci/check_kill_loop.py`.
- **Not covered:** `formal/MoCore/Saved.lean` models the format and proves schema checks at the specification level; byte-for-byte Rust-vs-Lean equivalence rests on test suites rather than a verified extraction.

---

## Control-plane and upgrade boundaries

- **Unix-socket administration (`--admin-socket`):** Public HTTP listeners serve only application routes—never operator views or code uploads. Live introspection (`mo-overview` through `mo-failures`), `mo fetch`, `mo query`, and `mo push` bind exclusively to an opt-in mode-`0600` Unix domain socket protected by OS filesystem permissions.
- **No unattended LLM live deploys:** Runtime self-healing happens at the process/supervision boundary; code fixes are verified offline (`mo replay --fix`, `mo diff`, `mo rehearse`) and reviewed by a human before deployment.
- **Confined upgrade compiler & `Command` children:** Live-upgrade compilation runs in a separate child process with an empty environment, no store file descriptors, resource limits, and Linux Landlock + seccomp confinement where supported (hosts without Landlock refuse live upgrades unless explicitly started with `--unconfined`). `Command` children likewise run without a shell under `allow_programs`, folder confinement, scrubbed environments, timeouts, and Linux Landlock + seccomp.
- **Write access to `--data DIR`:** Anyone with filesystem write access to a service's data folder holds full state and upgrade authority over that service.

---

## Gaps

These are explicitly not guaranteed today:

1. **Bundled C dependency (SQLite).** While all 17 Mo crates enforce `#![forbid(unsafe_code)]` with zero OpenSSL, SQLite itself is C code compiled into `rusqlite` (`bundled` feature), and `ring` contains assembly/C cryptographic primitives.
2. **OS-level confinement is child-only and Linux-specific.** Landlock + seccomp confine `Command` subprocesses and live-upgrade compiler children on Linux kernels with Landlock ABI 3+. On macOS and other non-Landlock hosts, `Command` enforces `allow_programs`, working-directory confinement, scrubbed environments, closed descriptors, and process-group timeouts without a kernel filesystem/syscall sandbox. `mo run`, `mo test`, and `mo serve` parent processes rely on Mo's capability boundary and safe-Rust runtime rather than OS sandboxing.
3. **No default CPU/time limit on `mo run`.** See P9.
4. **Verified bytecode, not verified compiler.** A bug in `mo-check`, `mo-lower`, or `mo-codegen` can produce verified bytecode that miscompiles the source semantics (a correctness bug, not a sandbox escape); the Lean differential suite and golden corpus guard against this.
5. **Process-kill tests do not prove power-loss durability.** SIGKILL recovery drills validate crash consistency across process death with an intact OS page cache, not arbitrary disk firmware reordering.
6. **Concurrent external filesystem mutations** are outside P8.

# Mo Architecture

This document describes the pure-Rust `mo` toolchain: its compiler pipeline, 17-crate workspace boundaries, shared contracts, Perceus memory model, bytecode verifier, virtual machine, service/API runtime, and testing strategy.

The [Mo Language Reference](../reference/README.md) defines correct language behavior and wins wherever any two documents disagree. The bytecode format and load-time verification rules live in [`bytecode.md`](bytecode.md), the capability and sandbox threat model in [`sandbox-properties.md`](sandbox-properties.md), the core library map in [`library.md`](library.md), and the formal Core specification in [`formal/README.md`](../../formal/README.md).

---

## 1. System overview

One pure-Rust executable, `mo`, compiles a whole Mo program from source to verified bytecode and executes it on its own virtual machine:

```text
┌────────┐  ┌───────────┐  ┌────────────┐  ┌──────────┐  ┌──────────┐  ┌────────────┐  ┌───────────┐  ┌───────┐
│ source ├─►│ mo-syntax ├─►│  mo-check  ├─►│ mo-lower ├─►│ mo-core  ├─►│ mo-codegen ├─►│mo-bytecode├─►│ mo-vm │
│ (.mo)  │  │  parser   │  │ types/caps │  │  to Core │  │ checker  │  │ RC + reuse │  │ verifier  │  │  VM   │
└────────┘  └───────────┘  └────────────┘  └────┬─────┘  └──────────┘  └────────────┘  └───────────┘  └───┬───┘
                                                │ Core text (v0.11)                                       │ outcomes
                                                ▼                                                         ▼
                                         ┌──────────────┐                                          ┌──────────────┐
                                         │ Lean 4 model │◄────────── differential tests ──────────►│ value, stop, │
                                         │   (mocore)   │                                          │  or failure  │
                                         └──────────────┘                                          └──────────────┘
```

- **Front end (`mo-syntax`, `mo-check`, `mo-lower`, `mo-core`):** Parses `.mo` files (preserving trivia for formatting), resolves modules (including embedded `lib/` and `mo.toml` path dependencies), checks types, abilities, capabilities, exhaustiveness, and `service`/`actor`/`api` declarations, lowers everything to monomorphic **Mo Core**, and validates the Core IR.
- **Mo Core (`formal/` + `crates/mo-core`):** The small, strict, monomorphic functional language every Mo feature lowers to. Defined and proven type-safe in Lean 4, exchanged via a lossless S-expression text format ([`formal/TEXT.md`](../../formal/TEXT.md)) so the Rust toolchain is continuously differential-tested against the Lean evaluator.
- **Bytecode compiler (`mo-codegen`):** Runs whole-program borrow inference, inserts precise Perceus reference counting and in-place memory reuse (`RESET`/`REUSE`/`FREE_TOKEN`/`SET_FIELD`), performs reachability pruning and register recycling, and emits `MOBC` v0.3 bytecode.
- **Loader, verifier & VM (`mo-bytecode`, `mo-heap`, `mo-prim`, `mo-host`, `mo-vm`):** Loads `.bc` bytecode, verifies structural integrity, static types, ownership/leak freedom, and spec-code isolation before running, and executes in isolated per-process heaps. The VM never sees source code, syntax trees, or Core.

### Shared contracts (`crates/mo-def/defs/`)

Four contract files in `crates/mo-def/defs/` are parsed at build time by `crates/mo-def/build.rs` into strongly typed Rust tables shared across every crate—no stage keeps a hand-duplicated copy:

| Contract | Source files | Shared between |
| --- | --- | --- |
| **Mo Core** | `formal/` (Lean 4), `crates/mo-def/defs/{core.h,ops.def}` | `mo-lower`, `mo-core`, `mo-codegen`, Lean `mocore` |
| **Bytecode** | `docs/design/bytecode.md`, `crates/mo-def/defs/opcodes.def` | `mo-codegen`, `mo-bytecode`, `mo-vm` |
| **Primitives** | `crates/mo-def/defs/prims.def`, `docs/design/library.md` | `mo-check`, `mo-lower`, `mo-codegen`, `mo-bytecode`, `mo-prim` |
| **Capabilities** | `crates/mo-def/defs/hostops.def`, `docs/design/library.md` | `mo-check`, `mo-codegen`, `mo-bytecode`, `mo-host`, `mo-vm` |

Two additional definition files in `crates/mo-def/defs/` pin shared constants: `limits.h` (every compiler and runtime ceiling), `codes.h` (every stable `E####` diagnostic code), and `version.def` (`MO_VERSION`).

---

## 2. Crate layout and dependency boundaries

Mo is organized as a **17-crate Cargo workspace** (Rust edition 2024). Every crate and binary enforces `#![forbid(unsafe_code)]`, `[lints] workspace = true` (denying `unwrap_used`, `todo`, `unimplemented`, `dbg_macro`, `await_holding_lock`, and warnings), **zero OpenSSL** (`libssl`/`libcrypto`), and **zero system `libsqlite3`** dynamic linkage.

```text
crates/
  mo-def/       Build-time generated tables from defs/: opcodes, prims, hostops, limits, codes, Core ops, version
  mo-base/      SourceMap, Spans, diagnostics, text/JSON renderers, CapCase-to-kebab module paths, budgets, UTF-8
  mo-syntax/    Lexer, recursive-descent parser with error recovery, trivia attachment, 100-column Wadler formatter
  mo-check/     Whole-program loader (embedded lib/, lazy on_use, mo.toml), type/ability/capability/exhaustiveness/api/actor/service checker
  mo-lower/     Monomorphization, derived abilities (Order/Text/Json/Sqlite.decode), CPS/join-point lowering to Mo Core
  mo-core/      Mo Core IR, lossless v0.11 S-expression codec, metadata validator, bidirectional Core typechecker
  mo-codegen/   Whole-program borrow inference, Perceus RC & reuse, register recycling, MOBC v0.3 emission
  mo-bytecode/  MOBC v0.3 binary reader, writer, disassembler, structural validator, type/ownership/spec verifier
  mo-heap/      Perceus reference-counted heap, Value/Cell representation, HeapBudget, insertion-ordered collections, Decimal/BigInt/Float, saved-state codec
  mo-prim/      Pure primitives: checked/wrapping/saturating math, text/bytes, Unicode 16.0, pure-Rust Crypto (sha2/hmac/aes-gcm/scrypt/subtle)
  mo-host/      Capability traits + real & fake implementations (Args, Env, Console, Files, Clock, Random, SecureRandom, Network/rustls, Sqlite/bundled rusqlite, Command/Landlock)
  mo-vm/        Bytecode interpreter, image initialization, call frames, defect reporting, deterministic Simulation/Actor runtime
  mo-test/      mo test engine: property input generation, shrinking, fake synthesis, recovery/promise simulation, --mutants, --strength
  mo-service/   Durable MCP service loop, SQLite snapshot/log store, operator views, immutable export/fetch, live upgrades, multi-worker api server
  mo-upgrade/   mo diff (authority/wire/state/behavioral contract checks), offline mo replay & mo query, mo rehearse
  mo-cli/       Unified `mo` CLI (17 commands, embedded docs & lib/), plus test helper binaries
  mo-fe/        Thin differential driver (Core execution, format, and check verification against Bison and Lean)

lib/            Standard library written in Mo (embedded into `mo`), including `lib/arvo/` web framework
runtime/        Embedded Mo HTTP/MCP protocol and operator-view modules compiled into `mo-service`
grammar/        Executable Bison LR(1) grammar (`mo.y`) proving syntactic unambiguity
formal/         Lean 4 formalization of Mo Core, type-safety proofs, and `mocore` oracle
models/         TLA+ specifications for export, failure capture, and live upgrade swap
```

### Strict layer isolation

- The **VM (`mo-vm`, `mo-bytecode`, `mo-heap`, `mo-prim`, `mo-host`)** never depends on `mo-syntax`, `mo-check`, `mo-lower`, `mo-core`, or `mo-codegen`.
- **`mo-syntax`** depends only on `mo-def` and `mo-base` (taking a display-width callback so it does not depend on `mo-prim`).
- **`mo-core`** depends only on `mo-def` and `mo-base`.
- **Limits:** Every input ceiling lives in `crates/mo-def/defs/limits.h`. Exceeding a compiler-input limit produces a diagnostic; exceeding a runtime limit (`--max-stack`, `--max-depth`, `--max-heap`, instruction budget) stops the process cleanly with a defect report.

---

## 3. Front end

### Parsing & formatting (`mo-syntax`)

The parser is hand-written recursive descent so diagnostics know which block is unfinished and where to resume cleanly after a syntax error. Every syntax node carries byte-offset spans; character-counted lines and columns are computed on demand when formatting diagnostics.

[`grammar/mo.y`](../../grammar/README.md) is the executable specification of Mo's syntax: Bison compiles it as conflict-free LR(1), and CI checks that `mo-syntax` and `mo-grammar` agree across the Language Reference, `lib/`, `examples/`, `tools/`, `services/`, and grammar tests.

`mo format` renders directly from the syntax tree with attached leading, trailing, and boundary-item comments using a 100-column Wadler pretty-printer (`Unicode16` display widths). Formatting is idempotent and verified on every tracked `.mo` file by `ci/check_format.py`.

### Module loading & checking (`mo-check`)

`mo-check` loads the whole program starting from the entry file:

1. **Embedded standard library (`lib/`):** Built into `mo-check` so the `mo` binary runs standalone anywhere without an external `lib/` directory (`--lib DIR` overrides for stdlib development). Core modules load automatically; optional modules (`Command`, `Crypto`, `Date`, `Http`, `Json`, `Unicode16`, `Api`, `Arvo.*`) load lazily on `use`.
2. **Packages (`mo.toml`):** When a `mo.toml` manifest sits in the entry module's directory or an ancestor, `use` resolves modules in the importing package first, then declared local path dependencies (`<dir>/src`, `<dir>/lib`, `<dir>`), then embedded `lib/`.
3. **Checking passes:** Resolves `expose`/`use` visibility, checks types and generic constraints, verifies built-in abilities (`Equal`, `Order`, `Hash`, `Text`, `Json`), verifies capability flow and spec-only boundaries, checks exhaustive `case` patterns, validates `service`, `actor`, and `api` declarations (route parameters, auth principals, exhaustive `status` mappings, wire visibility), and attaches machine-applicable edits for `mo fix`.

---

## 4. Lowering and Mo Core (`mo-lower`, `mo-core`)

`mo-lower` translates the checked program into **Mo Core**, eliminating surface conveniences:

- **Monomorphization:** Generic functions and types are instantiated per concrete type-argument list.
- **Derived abilities:** Generates Core functions for `Order` (`compare`), `Text` (`to_text`), `Json` (`to_json`, `from_json`, `from_json_strict`, `schema`), and `Sqlite.decode[T]`/`decode_one[T]`/`decode_optional[T]`.
- **Control flow to join points:** Loops (`for`, `while`), `break`, `continue`, `return`, and `try` lower to tail-position `join`/`jump` blocks. List, map, and set `for` loops lower to indexed/slot iteration rather than intermediate tuple allocations.
- **String interpolation folding:** Strips empty parts and folds concatenations left-to-right to preserve uniquely owned buffers for geometric growth.
- **Declarations for runtime metadata:** `service`, `actor`, and `api` blocks lower to compiler metadata descriptors and dispatch/job functions checked by `mo-core`.

`mo-core` validates every lowered program with its own bidirectional typechecker before codegen, and serializes/parses the lossless `.core` v0.11 S-expression format shared with Lean 4 (`formal/`).

---

## 5. Bytecode compiler & Perceus memory model (`mo-codegen`, `mo-heap`)

Mo uses **value semantics** with **Perceus-style reference counting and in-place reuse**—giving deterministic deallocation and fast in-place updates without a tracing garbage collector or user-facing lifetimes:

1. **Whole-program borrow inference (`mo-codegen`):** Infers which parameters can be passed borrowed instead of owned, avoiding unnecessary `DUP`/`DROP` pairs across call boundaries.
2. **Precise ownership placement:** Inserts `DUP` before shared uses and `DROP` immediately after last uses so values are freed as early as possible.
3. **Opportunistic in-place reuse:** Emits `RESET` (returns a reuse token if `refcount == 1`, otherwise decrements and yields null), `REUSE` (reinitializes the slot in place or allocates fresh), `FREE_TOKEN`, and `SET_FIELD` (in-place field mutation on uniquely owned records). Nested field updates (e.g., `s.items = s.items.append(x)`) clear the old field reference before evaluating the RHS so `refcount == 1` is preserved.
4. **Register recycling & reachability pruning:** Prunes unreachable functions and recycles exact-type registers locally per basic block without emitting cold-branch prologue writes.

In `mo-heap`, every value fits in a compact `Value` representation managed inside a per-process `Heap` governed by `HeapBudget` (`--max-heap`, default 1 GiB). Reference-counted heap objects include `Text`, `Bytes`, `BigInt`, tuples, `List`, insertion-ordered `Map` and `Set`, records, boxed choice cases, closures, and capability handles. Exact 128-bit `Decimal` arithmetic (`mo-heap/src/decimal.rs`) uses 256-bit intermediate products with ties-to-even rounding; float formatting matches shortest round-trip decimal rules.

---

## 6. Bytecode verification & VM (`mo-bytecode`, `mo-vm`)

Every bytecode image (`MOBC` v0.3, specified in [`bytecode.md`](bytecode.md)) is validated and verified by `mo-bytecode` before `mo-vm` executes a single instruction—whether freshly compiled or loaded from a `.bc` file on disk (`mo run FILE.bc`):

- Structural decoding and bounds checks against `limits.h`
- Static type verification of every instruction, primitive, host operation, call, closure, and switch
- Control-flow graph reachability and jump-target alignment
- Abstract-interpretation **ownership dataflow** proving every owned reference is consumed exactly once on every path, with zero double-frees, zero use-after-frees, zero leaks, and zero reads of uninitialized registers
- Global initialization rank ordering (initialized once in dependency order)
- **Spec-code isolation:** non-spec code cannot call, tail-call, or capture spec functions, fake capability constructors, or `fail`

The VM (`mo-vm`) interprets verified bytecode in a per-process frame stack with bounded `--max-stack` (default 8 MiB), `--max-depth` (default 10,000), `--max-heap`, and optional instruction budgets (`--max-work` / service handler budgets). On a defect (overflow, division by zero, `stop`, limit exceeded), `mo-vm` re-runs the verifier's readability analysis to print only live, readable locals in the defect report and frees the process heap whole.

---

## 7. Capabilities & host boundary (`mo-host`, `mo-prim`)

All outside-world effects enter exclusively through capability objects dispatched via `host` instructions (`crates/mo-def/defs/hostops.def`):

- **Zero ambient authority:** `mo-cli` constructs real capabilities solely for the parameters declared on `main` (or wired into an `api` block).
- **Monotonic attenuation:** `Files.within`, `ReadFiles.within`, `Files.read_only`, `Network.allow_hosts`, `Command.within`, and `Command.allow_programs` only narrow an existing handle.
- **Directory containment:** `Files`, `ReadFiles`, `Files.File`, `Sqlite`, and `Command` normalize paths lexically and resolve beneath their root descriptor (`openat2` `RESOLVE_BENEATH` on Linux or component-wise `O_NOFOLLOW` traversal), rejecting `..` escapes and escaping symlinks.
- **Pure-Rust TLS & crypto:** `Network.connect_secure` uses `rustls` (TLS 1.2+ with `ring` and native/webpki root certificates); `Crypto` primitives (`mo-prim/src/crypto.rs`) use pure-Rust `sha2`, `hmac`, `aes-gcm`, `scrypt`, `subtle`, and `zeroize`; `SecureRandom` uses `getrandom`. Zero OpenSSL linkage.
- **Bundled SQLite:** `Sqlite` and durable service stores use `rusqlite` with `bundled` SQLite, WAL mode, `PRAGMA foreign_keys = ON`, and an authorizer blocking `ATTACH`, `DETACH`, `VACUUM INTO`, extension loading, and disk temp stores.
- **Child confinement:** `Command` subprocesses and live-upgrade compiler children run without a shell, with scrubbed environments, explicit program/directory allowlists, resource limits, and Linux Landlock + seccomp confinement where supported.
- **Deterministic fakes:** Every capability implements a matching spec-only in-memory fake (`Console.fake`, `Files.in_memory`, `Clock.fixed`, `Random.seeded`, `SecureRandom.seeded`, `Network.fake`, `Sqlite.fake`/`in_memory`, `Command.fake`, `Simulation`, `Actor.start`, `ApiTest.send`).

---

## 8. Services, actors, APIs & upgrades (`mo-service`, `mo-upgrade`)

- **Declarative `api` servers (`mo-service/src/api_server.rs`):** Multi-worker HTTP/1.1 keep-alive server (`--bind`, `--port`, `--workers`, `--watch`, `--data`). Each request runs in an isolated `mo-vm` process with per-worker SQLite pooling, automatic rollback on handler stops, confined static mounts (`ETag`/`304`), OpenAPI 3.1 + `/docs`, sanitized single-line request logs, and background `job` execution against the durable `arvo_jobs` table.
- **In-process `actor`s (`mo-vm/src/simulation.rs` + `mo-service`):** Serialized state machines with private state, periodic timers, and cross-heap message copying via the saved-state codec so caller and actor heaps never share mutable state.
- **Durable MCP `service`s (`mo-service`):** Single-writer state machine persisted to SQLite (`service.sqlite`) with snapshots and recovery logs (`docs/design/saved-state.md`). HTTP policy, JSON-RPC validation, wire encoding, and operator-view rendering run in embedded Mo modules (`runtime/*.mo`). Public HTTP exposes only declared service tools; `--admin-socket PATH` (mode `0600`) exposes the nine read-only operator views (`mo-overview`, `mo-state`, `mo-mailbox`, `mo-timers`, `mo-memory`, `mo-supervisor`, `mo-log`, `mo-export`, `mo-failures`) and three upgrade tools (`mo-upgrade-open`, `mo-upgrade-page`, `mo-upgrade-commit`).
- **Offline diagnosis & diffs (`mo-upgrade`):**
  - `mo history`, `mo replay` (with `--fix` original-first verdict checking), and `mo query` operate on stopped data folders or verified `.moex` exports (`mo fetch`).
  - `mo rehearse` tests candidate upgrades and typed migrations offline against state exports.
  - `mo diff` compares two program versions (defaulting `OLD` to Git `HEAD`) across capability authority, public/wire interfaces, persistent state schemas, and behavioral contracts by executing `OLD`'s compiled `promise`s and `examples` against `NEW`.

---

## 9. Testing & verification strategy

Every check is orchestrated by [`ci/check.sh`](../../ci/check.sh) in six independent groups:

| Group | Command | What it verifies |
| --- | --- | --- |
| `rust` | `bash ci/check.sh rust` | `cargo fmt`, `cargo clippy -- -D warnings`, all workspace unit/integration & doc tests (including 48-directory `fuzz/corpus/` replay in `crates/mo-cli/tests/corpus_replay.rs` and Linux child confinement tests in `crates/mo-cli/tests/confinement_tests.rs`), `rustdoc -D warnings`, `#![forbid(unsafe_code)]` audit, `ldd` dynamic linkage audit (zero OpenSSL, zero system `libsqlite3`), release builds, and Rust VM/bytecode fuzz smoke tests. |
| `drivers` | `bash ci/check.sh drivers` | Black-box CLI goldens & flag matrix (`ci/check_cli.py`), `docs/agent-guide.md` claim verification (`ci/cli_guide.py`), reference example execution (`ci/check_reference_examples.py`), golden corpus (`ci/check_golden.py`), full error-code coverage (`ci/check_errors.py`), formatter idempotence on every tracked `.mo` file (`ci/check_format.py`), `lib/`, `tools/`, and `services/` tests, crypto cross-check against Python stdlib, live HTTP/TLS/service/upgrade/fetch/offline/kill-loop tests, and `< 0.1 s` check-time budget. |
| `static` | `bash ci/check.sh static` | Executable Bison grammar over all corpora (`make -C grammar grammar`), `docs/design/library.md` vs. `prims.def`/`hostops.def`/`ops.def` (`ci/library_map.sh`), Zed & Cursor/VS Code editor grammars (`ci/check_editors.py`), and reference conformance inventory (`ci/check_conformance.py`). |
| `lean` | `bash ci/check.sh lean` | Lean 4 `MoCore` build and proof checks, pinned-axiom audit (`ci/check_lean_axioms.sh`), Core text fixture parity, defect-replay parity, and pure-Rust vs. Lean Core differential testing. |
| `bench` | `bash ci/check.sh bench` | Frozen benchmark v0, Mo-written tools (`linkcheck`, `notioncopy`, `tally`—including live Markdown link checking across the entire repo), and learnability calculators. |
| `models` | `bash ci/check.sh models` | Finite TLA+ model checking (TLC) for export generation safety/progress, failure capture, and live upgrade swap, including negative controls. |

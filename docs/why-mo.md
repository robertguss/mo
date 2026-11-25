# Why Mo

Mo is a programming language and runtime I built for **living software**—software written largely by AI agents, read and verified by humans, and operated, inspected, and evolved while it runs.

This document explains why I built Mo, the languages and research it draws from, and the concrete reasoning behind its features.

---

## Why I built Mo

AI agents now write a growing share of real software, yet every mainstream language and runtime was designed for a world where humans write code by hand and humans babysit production servers:

- **Dynamic scripting languages** (Python, JavaScript) are fast to iterate in, but let bugs hide behind `None`/`undefined`, untyped dictionaries, implicit global I/O, and runtime exceptions until production traffic hits them.
- **Systems languages** (Rust, C, C++) provide strong guarantees and single-binary performance, but spend enormous human and agent effort fighting lifetimes, borrow-checker gymnastics, or memory-safety hazards when writing everyday APIs, ETL tools, and services.
- **Managed enterprise and actor runtimes** (JVM, BEAM, Go) solve parts of concurrency or operations, yet still rely on ambient authority (any dependency can touch the filesystem, read env vars, or open a socket), lack built-in specification checking, and rarely separate pure domain logic from I/O cleanly by construction.

I wanted a single language and single-binary runtime with a different set of trade-offs:

1. **Written by agents, verified by humans.** Even when an agent writes 100% of the code, a human still reads the code, reviews the diffs, and owns the blast radius. Following Yukihiro Matsumoto's philosophy for Ruby—*developer happiness*—`.mo` files should be calm, concise, predictable, and genuinely pleasant to read. Any rule that forces prose bureaucracy, duplicated helpers, or awkward idioms is treated as a design bug in Mo.
2. **Rust's guarantees without Rust's friction.** Algebraic types, exhaustive pattern matching, explicit `Option` and `Result`, checked arithmetic, exact `Decimal` money math, explicit capability authority, sub-second whole-program checks, and in-place memory reuse—with **no borrow checker** and **no garbage-collection pauses**.
3. **A runtime built to be inspected and evolved.** Services shouldn't be opaque black boxes that require ad-hoc SSH debugging or risky downtime deploys. Isolation, deterministic replay, pure offline queries, structural diffing, and typed live upgrades should be part of the platform.
4. **Security by construction, not convention.** When agents write and run tools, code must only reach the exact directories, hosts, databases, and binaries it was explicitly handed—never ambient filesystem or network access.

Before this repository, I explored these ideas through 13 research experiments and Lean proofs of memory-reuse rules (archived in [robertguss/mo-v2](https://github.com/robertguss/mo-v2)). Mo is the resulting language, compiler, verifier, and runtime—implemented from scratch in pure safe Rust.

---

## What Mo is based on

Mo doesn't try to be novel for novelty's sake. It combines proven ideas from five lineages into one cohesive whole:

| Influence | What Mo takes from it | How Mo adapts it |
| --- | --- | --- |
| **Elixir & the BEAM** | Share-nothing per-process heaps, supervision, fault containment ("let it crash"), actors, live introspection, and hot code loading. | Mo pairs concurrent HTTP `api` servers with stateful in-process `actor`s and durable MCP `service`s inside one standalone binary—with static typing, deterministic instruction/memory budgets, and verified bytecode. |
| **Rust** | Algebraic data types (`struct`, `enum`), exhaustive matching, `Option`/`Result` error handling, explicitness, single-binary tooling, and memory safety. | Mo gives user code those guarantees without lifetimes or a borrow checker, while the toolchain itself is written in `#![forbid(unsafe_code)]` Rust. |
| **Koka & Perceus** | Pure value semantics compiled with whole-program borrow inference, precise reference counting, and opportunistic in-place memory reuse. | Writing clean value updates (`after.balance -= amount`, `items = items.append(x)`) reuses uniquely owned memory in place (`RESET`/`REUSE`/`SET_FIELD`) with zero GC pauses. |
| **Elm** | Helpful, single-cause compiler diagnostics written in plain language, and a small, disciplined language surface. | Every Mo error has a stable `E####` code, exact source span, rule citation, and structured `--json` output; single-fix diagnostics can be applied automatically with `mo fix`. |
| **Object-Capability Security** (Capsicum, WASI, `cap-std`) | Zero ambient authority; authority is passed as values and can only be narrowed, never amplified. | `main` declares every capability it needs; `within`, `read_only`, `allow_hosts`, and `allow_programs` attenuate capabilities monotonically, and every capability has an exact in-memory fake for tests. |

On top of those five influences, Mo's internal core language (**Mo Core**) is formalized in **Lean 4** ([`formal/`](../formal/README.md)) with mechanized type-safety proofs, and its grammar is verified conflict-free by Bison ([`grammar/`](../grammar/README.md)).

---

## Features and the reasoning behind them

### 1. Calm, unambiguous syntax and one canonical format

- **Blocks end with `end`, no semicolons, indentation is cosmetic.** Indentation-sensitive syntax frequently trips up automated edits and diff merges; C-style braces encourage formatting wars. Keyword/`end` blocks stay readable for humans and resilient for agents.
- **One unambiguous grammar (`grammar/mo.y`).** Bison compiles Mo's grammar with zero shift/reduce or reduce/reduce conflicts. If a syntax change introduces ambiguity, the build rejects it.
- **One formatter (`mo format`).** A 100-column formatter preserves comments deterministically so humans and agents never waste review cycles on style.
- **No `null`, no exceptions, no implicit coercions, no warnings.**
  - Missing values use `Option(T)` (`Some`/`None` with `x or default`).
  - Recoverable failures use `Result(T, E)` with `try` for propagation and `case` for handling; dropping a `Result` is a compile error (`E0412`).
  - Impossible states use `stop "why this cannot happen"` (which produces a structured defect report with readable local variables rather than an unlabeled panic).
  - Integer overflow stops cleanly instead of silently wrapping; wrapping and saturating operations exist explicitly (`wrapping_add`, `saturating_add`).
  - Either a problem is a compile error or it isn't reported—there are no warning lists to ignore.

### 2. Compiler diagnostics built for fast repair loops

- **Stable `E####` codes and `--json` output.** Every diagnostic identifies its exact byte/line/column span, cites the Language Reference rule, reports errors in source order without cascading noise, and includes machine-applicable text edits when there is a single unambiguous fix (`mo fix`).
- **Sub-100ms whole-program checks.** Fast feedback loops matter enormously for both humans and agents; `mo check` checks a full program in milliseconds.
- **Self-documenting binary (`mo guide`).** The Language Reference (`mo guide --reference`) and operational guide (`mo guide`) are embedded directly inside the `mo` binary, so any agent or developer with the binary has the exact specification for that build offline.

### 3. Specifications live inside the language

Tests hidden in separate directories drift from the code they describe. Mo puts executable intent right at the top of a function, `actor`, or `service`:

- **`examples`** check concrete input/output cases right next to the signature.
- **`promise`** blocks state universal invariants (including `implies`) over inputs and `result` (or `before`/`after` state and `now` on `actor`s and `service`s). `mo test` generates deterministic typed inputs, shrinks failing counterexamples (including builder-sequence shrinking for opaque types), and **automatically rejects vacuous promises** (tautologies or `a implies b` rules whose antecedent is never true).
- **`test`** blocks exercise multi-step workflows, capability interactions, HTTP routes, and service simulations.
- **Automatic crash-recovery checks.** For every `service` and `actor`, `mo test` automatically synthesizes sequences of calls, clock advances, crashes, and restarts—verifying that persisted state recovers identically without the author writing a single recovery test.
- **Opt-in mutation auditing (`mo test --mutants`).** Plain `mo test` runs checks fast with zero ceremony. When auditing test thoroughness, `--mutants` mutates comparisons, booleans, arithmetic, and constants to reveal untested branches (`E1439`).
- **Structural and behavioral diffs (`mo diff`).** Comparing a candidate program against Git `HEAD` (or an older file), `mo diff` reports four architectural deltas: capability authority changes (including narrowings), exposed/wire API changes, persistent state schema changes, and a **behavioral contract check** that runs the *old* program's compiled `promise`s and `examples` directly against the *new* implementation—catching silent semantic regressions even when private helpers are refactored.

### 4. Object-capability security and deterministic fakes

In Mo, a function cannot perform I/O unless a capability value is passed to it, and `main` receives only the capabilities listed in its signature:

- `Args`, `Env`, `Console`
- `Files`, `ReadFiles`, `Files.File` (strictly confined inside their directory via `openat2` `RESOLVE_BENEATH` on Linux or component-wise `O_NOFOLLOW` traversal—never escaping via `..` or symlinks)
- `Clock`, `Random`, `SecureRandom`
- `Network`, `Connection` (TCP + TLS 1.2+ via `rustls` with system roots, plus `network.allow_hosts([...])` host allowlisting)
- `Sqlite`, `Sqlite.Database` (bundled SQLite with WAL mode, `PRAGMA foreign_keys = ON`, directory confinement, and SQLite authorizer blocking `ATTACH`/`VACUUM INTO`/extensions)
- `Command` (shell-free subprocess execution with `command.within(dir)` and `command.allow_programs([...])`, backed by Linux Landlock + seccomp child confinement where available)

**Why this matters:**
1. **Least privilege is visible in the type signature.** A reviewer can see at a glance whether a module can touch the network, disk, or subprocesses—and `mo diff` flags any authority expansion automatically.
2. **Narrowing is monotonic.** Passing `files.within("reports").read_only` or `network.allow_hosts(["api.github.com:443"])` guarantees the callee can never widen its reach back out.
3. **Testing is 100% deterministic and zero-I/O.** Every capability has a spec-only in-memory fake (`Console.fake()`, `Files.in_memory(...)`, `Clock.fixed(...)`, `Random.seeded(...)`, `SecureRandom.seeded(...)`, `Network.fake(...)`, `Sqlite.in_memory()`, `Command.fake(...)`). Spec-only fakes are rejected by the bytecode verifier outside spec code, so production code can never forge a fake capability.

### 5. Pure value semantics with Perceus in-place reuse

Sharing mutable references across threads and functions is the root cause of aliasing bugs, spooky action at a distance, and borrow-checker complexity. Mo chooses **value semantics**: every `struct`, `List`, `Map`, `Set`, and `Text` behaves as an independent value.

To make value semantics fast without garbage-collection pauses, `crates/mo-codegen` implements **Perceus-style reference counting and in-place reuse**:

- Whole-program **borrow inference** eliminates redundant reference-count increments and decrements.
- When a `var` holding a uniquely owned value (`refcount == 1`) is updated—such as `after.balance -= amount`, `items = items.append(x)`, `acc = "#{acc}#{piece}"`, or nested field updates—the compiler emits `RESET`/`REUSE`/`SET_FIELD` instructions and geometric growth paths so memory is mutated in place with $O(1)$ amortized cost.
- `for` loops iterate lists, maps, and sets directly without intermediate tuple boxing.

You write pure, readable value transformations and get predictable, low-latency memory behavior automatically.

### 6. Exact numbers, dates, and compiler-derived abilities

Real-world backend and data-wrangling code suffers constantly from floating-point money bugs and JSON boilerplate. Mo addresses both directly:

- **Exact 128-bit `Decimal`** (`±(2^127 - 1) × 10^-18`, 256-bit intermediate products, ties-to-even division) is a first-class built-in type for money, invoices, and token budgets—floats never need to touch currency.
- **Checked 64-bit `Int`, arbitrary-precision `BigInt`, IEEE 754 `Float`, `Time`/`Duration`, and validated Gregorian `Date`** cover everyday domain math cleanly.
- **Compiler-derived abilities** (`Equal`, `Order`, `Hash`, `Text`, `Json`) work automatically on `struct`s and `enum`s with no derive-macro ceremony.
- **Pragmatic `Json` decoding:** `Json.decode[T]` ignores unknown extra object fields by default—making it effortless to decode wide third-party REST payloads into small Mo structs—while incoming `api` request bodies use `Json.decode_strict[T]` to reject typos with structured `422` errors. Types can override `to_json`/`from_json` when custom wire formats are needed.

### 7. Separate constructs for separate physics: `api`, `actor`, and `service`

Rather than forcing every workload through one abstraction, Mo gives three distinct runtime shapes their own first-class syntax:

- **`api ... end`** — Concurrent, multi-worker HTTP/1.1 REST and full-stack web servers. Each HTTP request runs in its own isolated VM process and heap. Route parameters, query strings, strict JSON bodies, `scope` prefixes, confined `static` asset mounts (`ETag`/`304`), type-driven `auth` principals (`Principal` vs. `Option(Principal)`), background `job`s, and exhaustive `status` error mappings are all checked statically at `mo check` time—and `/openapi.json` plus interactive `/docs` are generated automatically. `ApiTest.send` tests routes end-to-end in memory with zero sockets.
- **`actor Name ... end`** — Composable, in-process BEAM-style state machines with private state, serialized message handlers, periodic `every` timers, and machine-checked `promise` invariants. An `api` server can hold `actor` capabilities (such as rate limiters, SSE hubs, token budgeters, or batch writers) inside the same binary—eliminating external Redis/Celery infrastructure for many workloads.
- **`service ... end`** — Durable, single-writer stateful services exposed over authenticated MCP/HTTP with SQLite snapshot + recovery-log persistence, crash supervision, live operator introspection, failure capture, offline replay, and typed live upgrades.

On top of `api` + `Sqlite` + `Crypto` + `actor`, the pure-Mo **[Arvo](arvo.md)** web framework (`lib/arvo/`) provides sessions, CSRF, flash messages, form/multipart parsing, changesets, SQLite migrations, audit logs, durable background jobs, and Inertia v2 integration—all compiled into one standalone binary.

### 8. Operating living software safely

Mo draws a hard security line between **runtime self-healing** and **code changes**:

- **Self-healing happens at the process boundary.** A crashing handler or runaway request hits its instruction or memory budget, aborts its isolated process, rolls back uncommitted state, captures a bounded failure record, and leaves the server running.
- **No unattended LLM live deploys.** Mo deliberately avoids wiring untrusted crash payloads directly to autonomous production deployment (preventing crash → prompt-injection → RCE chains). Instead, an agent diagnoses and verifies a fix offline via `mo replay --failure ID --fix` and `mo diff`, and a human approves deployment.
- **Control-plane isolation.** Public HTTP listeners serve only application routes. Operator inspection (`mo-overview`, `mo-state`, `mo-mailbox`, `mo-timers`, `mo-memory`, `mo-supervisor`, `mo-log`, `mo-export`, `mo-failures`), `mo fetch`, `mo query`, and `mo push` live upgrades run over an opt-in local Unix domain socket (`--admin-socket`, mode `0600`) protected by OS filesystem permissions.
- **Typed live upgrades (`mo push` & `mo rehearse`).** Upgrading a running `service` verifies state schema compatibility, runs typed pure migrations in a confined compiler child, rehearses offline against exported state snapshots (`mo rehearse`), and swaps atomically at a message boundary with full crash recovery.

### 9. Pure safe Rust, verified bytecode, and formal core

Mo's capability security is only as strong as the runtime underneath it:

- **100% pure safe Rust.** All 18 crates in the workspace enforce `#![forbid(unsafe_code)]`, strict Clippy lints (`unwrap_used = "deny"`, `todo = "deny"`, warnings as errors), **zero OpenSSL**, and **zero system `libsqlite3`** dynamic linkage (using `rustls`, RustCrypto crates, and bundled SQLite).
- **Verified bytecode (`MOBC` v0.3).** The VM never sees source code or ASTs. Every loaded `.bc` image passes structural validation, static type checking, Perceus ownership/leak verification, and spec-code isolation checks before a single instruction runs (`docs/design/sandbox-properties.md`).
- **Single-binary portability.** The standard library (`lib/`), embedded runtime (`runtime/`), Language Reference, and guide are compiled into `mo`. `mo emit` produces portable verified `.bc` files that `mo run` executes directly, and `mo install` packages any Mo CLI tool onto `$PATH`.
- **Formal foundation.** Mo Core is defined and proven type-safe in Lean 4 (`formal/`), with differential tests checking the pure-Rust compiler and VM against the Lean evaluator, plus TLA+ models (`models/`) verifying export, failure-capture, and live-swap protocols.

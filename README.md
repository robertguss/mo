# Mo

**An open-source programming language and single-binary runtime for living software.**

Mo is designed for a world where AI agents write a growing share of code, humans read and verify the logic, and services are inspected, diagnosed, and evolved while they run.

> **Status: v0.3.0 (pre-release).** Mo is implemented in 100% pure safe Rust (`#![forbid(unsafe_code)]`, zero OpenSSL, zero system `libsqlite3`). The language, compiler, verifier, VM, testing engine, web framework, and live-upgrade runtime work end to end. Contributions are not open yet—star or watch the repository to follow along.

---

## Why I built Mo

Mainstream languages force an uncomfortable choice: scripting languages make iteration fast but hide bugs behind `null`, exceptions, and ambient I/O; systems languages provide safety and speed at the cost of borrow-checker friction; actor runtimes solve fault isolation but still let any dependency reach the filesystem or network silently.

I built Mo to bring four ideas together in one cohesive tool:

- **Written by agents, verified by humans.** Inspired by Matz's philosophy of developer happiness, `.mo` files stay calm, concise, and unambiguous—a joy for humans to review and straightforward for agents to generate and fix.
- **Rust's guarantees without Rust's friction.** Algebraic types, exhaustive pattern matching, explicit `Option`/`Result`, exact 128-bit `Decimal` arithmetic, and Perceus in-place memory reuse—with **no borrow checker** and **no GC pauses**.
- **Security by construction.** Zero ambient authority. `main` declares every capability it needs, capabilities can only be narrowed (`within`, `read_only`, `allow_hosts`, `allow_programs`), and every capability has a deterministic in-memory fake for zero-I/O testing.
- **A runtime built for operations.** Isolated per-request heaps, composable in-process `actor`s, durable MCP `service`s, live Unix-socket operator views, offline failure replay, structural & behavioral diffing (`mo diff`), and typed live upgrades.

Read the full rationale and design decisions in **[Why Mo](docs/why-mo.md)**.

---

## What Mo is based on

Mo stands on proven foundations from five lineages plus a formal core:

| Influence | What Mo draws from it |
| --- | --- |
| **Elixir & the BEAM** | Share-nothing per-process heaps, instruction/memory budgets, supervision ("let it crash"), composable `actor`s, live introspection, and hot code upgrades. |
| **Rust** | Algebraic data types (`struct`/`enum`), exhaustive `case` matching, explicit `Option`/`Result` with `try`, checked arithmetic, single-binary distribution, and a `#![forbid(unsafe_code)]` Rust toolchain. |
| **Koka & Perceus** | Pure value semantics compiled with whole-program borrow inference, precise reference counting, and opportunistic in-place memory reuse (`RESET`/`REUSE`/`SET_FIELD`). |
| **Elm** | Helpful, single-cause diagnostics (`E####`) with exact source spans, zero warnings, structured `--json` output, and automatic fixes via `mo fix`. |
| **Object-Capability Security** | Zero ambient authority (following Capsicum, WASI, and `cap-std`): explicit capabilities, monotonic narrowing, and kernel-enforced directory/process confinement (`openat2` `RESOLVE_BENEATH`, Linux Landlock + seccomp). |
| **Formal Verification** | Mo Core is defined and proven type-safe in **Lean 4** ([`formal/`](formal/README.md)), the syntax is proven unambiguous by **Bison** ([`grammar/`](grammar/README.md)), and service protocols are model-checked in **TLA+** ([`models/`](models/README.md)). |

The earlier research experiments and Lean proofs of Mo's memory-reuse rules are archived in [robertguss/mo-v2](https://github.com/robertguss/mo-v2).

---

## A taste of Mo

### Pure domain logic with executable `promise`s and `examples`

```mo
module Account
expose Account, AccountError(..), open, deposit, withdraw

struct Account
  owner: Text
  balance: Decimal
end

enum AccountError
  InvalidAmount
  InsufficientFunds(short_by: Decimal)
end

fn open(owner: Text) : Account
  Account(owner: owner, balance: 0)
end

fn deposit(account: Account, amount: Decimal) : Result(Account, AccountError)
  return Error(InvalidAmount) if amount <= 0
  var after = account
  after.balance += amount
  Ok(after)
end

fn withdraw(account: Account, amount: Decimal) : Result(Account, AccountError)
  promise
    result is Ok(after) implies after.balance >= 0
    result is Ok(after) implies after.balance == account.balance - amount
  end

  examples
    withdraw(Account(owner: "ada", balance: 100.00), 30.00) ==
      Ok(Account(owner: "ada", balance: 70.00))
    withdraw(Account(owner: "ada", balance: 100.00), 100.01) ==
      Error(InsufficientFunds(short_by: 0.01))
  end

  return Error(InvalidAmount) if amount <= 0
  return Error(InsufficientFunds(short_by: amount - account.balance)) if amount > account.balance
  var after = account
  after.balance -= amount
  Ok(after)
end
```

### Declarative HTTP `api` + in-process `actor` + zero-socket tests

```mo
module CounterApi
use Api

struct Count
  value: Int
end

actor Counter
  state Count = Count(value: 0)
  calls add, current

  promise on add
    after.value == before.value + n
  end
end

fn add(s: Count, _now: Time, n: Int) : (Count, Int)
  (Count(value: s.value + n), s.value + n)
end

fn current(s: Count, _now: Time) : Int
  s.value
end

api
  actor counter: Counter
  post "/add/:n" post_add
  get  "/count"  get_count
end

fn post_add(counter: Counter, n: Int) : Int
  counter.add(n)
end

fn get_count(counter: Counter) : Int
  counter.current
end

test "HTTP routes and actor state run in memory with zero sockets"
  counter = Actor.start(Counter)
  assert ApiTest.send(CounterApi, Api.post("/add/4"), counter).text == "4"
  assert ApiTest.send(CounterApi, Api.get("/count"), counter).text == "4"
end
```

`mo test` runs the `examples`, generates typed inputs for the `promise`s (rejecting vacuous rules automatically), executes the `test` block in memory, and automatically runs crash-and-recovery sequences against `Counter`—all in milliseconds.

---

## Key features & design reasoning

| Area | What Mo provides | Why |
| --- | --- | --- |
| **Calm, unambiguous syntax** | Keyword/`end` blocks, no semicolons, one 100-column formatter (`mo format`), Bison LR(1) conflict-free grammar, no `null`, no exceptions, no warnings. | Eliminates ambiguity and style churn so humans review logic quickly and agents converge in fewer iterations. |
| **Executable intent** | Built-in `examples`, code-first `promise` blocks with automatic non-vacuity checking, `test`, deterministic shrinking, opt-in `mo test --mutants`, and `mo diff`. | Keeps specifications next to code so tests don't drift; `mo diff` verifies old contracts against refactored code automatically. |
| **Object capabilities** | Explicit `Args`, `Env`, `Console`, `Files`, `ReadFiles`, `Clock`, `Random`, `SecureRandom`, `Network` (TCP + TLS 1.2+), `Sqlite`, and `Command` capabilities with monotonic narrowing and in-memory fakes. | Least privilege is visible in function signatures, enforced at runtime, and 100% deterministic to test without mocks or containers. |
| **Perceus memory reuse** | Pure value semantics compiled with whole-program borrow inference, precise reference counting, and in-place reuse (`RESET`/`REUSE`/`SET_FIELD`). | Fast, predictable performance with zero GC pauses and zero borrow-checker annotations. |
| **Built-in abilities & exact math** | Compiler-derived `Equal`, `Order`, `Hash`, `Text`, and `Json` (lenient external `Json.decode[T]`, strict `api` input validation); 128-bit `Decimal`, checked 64-bit `Int`, `BigInt`, and `Date`. | Removes JSON/serialization boilerplate and prevents silent floating-point currency or integer-overflow bugs. |
| **Web APIs, Actors & Arvo** | Multi-worker `api ... end` servers with automatic OpenAPI 3.1 + `/docs`, type-driven `auth`, exhaustive `status`, static mounts, background `job`s, composable `actor`s, and the pure-Mo **[Arvo](docs/arvo.md)** web framework (Inertia v2, sessions, CSRF, migrations, jobs). | One standalone binary replaces an HTTP server, OpenAPI generator, job runner, and in-memory state store—zero Node or Redis in production. |
| **Durable services & live ops** | `service ... end` over MCP/HTTP with SQLite snapshot/log persistence, Unix-socket `--admin-socket` operator views, immutable `mo fetch`, offline `mo replay` & `mo query`, and typed live upgrades (`mo push`, `mo rehearse`). | Separates runtime self-healing from code deployment—agents diagnose failures offline and humans review diffs before live upgrades. |
| **Pure safe Rust & verified bytecode** | 18-crate `#![forbid(unsafe_code)]` Cargo workspace, zero OpenSSL, zero system `libsqlite3`, verified `MOBC` v0.3 bytecode, embedded `lib/` and docs, `mo emit` and `mo install`. | Memory-safe runtime by construction, portable standalone binaries, and bytecode verified before execution. |

---

## Quick start

Building requires a Rust toolchain (`cargo`, edition 2024):

```sh
git clone https://github.com/robertguss/mo
cd mo
cargo build --release --bin mo
```

Or install `mo` directly onto your `$PATH`:

```sh
cargo install --path crates/mo-cli
```

Try it out:

```sh
./target/release/mo --version
./target/release/mo guide                     # embedded build guide
./target/release/mo guide --reference         # embedded Language Reference
./target/release/mo run examples/greet.mo Ada
./target/release/mo test examples/account.mo
./target/release/mo test examples/arvo-starter/main.mo
```

Package any Mo CLI tool onto `$PATH` as pre-verified bytecode with `mo install`:

```sh
./target/release/mo install tools/moloc/main.mo
moloc --help
```

Set up syntax highlighting and format-on-save for **Zed** or **Cursor / VS Code** via [`editors/README.md`](editors/README.md).

---

## The `mo` CLI

| Command | What it does |
| --- | --- |
| `mo check FILE` | Whole-program type, ability, capability, and exhaustiveness check (`--json`, `--emit=core`, `--emit=bytecode`). |
| `mo run FILE [ARGS...]` | Compile and run a `.mo` program—or load, verify, and run a compiled `.bc` bytecode file directly (`--root`, `--state-dir`). |
| `mo test FILE` | Run `examples`, `promise`s (with non-vacuity checks and shrinking), `test`s, and automatic `service`/`actor` recovery checks (`--mutants`, `--only`, `--in`, `--list`, `--json`). |
| `mo format FILE...` | Canonical 100-column formatter (`--stdin` for editor integration). |
| `mo fix FILE...` | Apply unambiguous single-fix compiler suggestions automatically. |
| `mo emit FILE -o OUT.bc` | Compile and write portable, verified `MOBC` v0.3 bytecode. |
| `mo install FILE` | Compile a `.mo` entry point to verified `.bc` and install an executable launcher onto `~/.local/bin`. |
| `mo serve FILE` | Serve a declarative `api` server (`--bind`, `--port`, `--workers`, `--watch`, `--data`) or durable MCP `service` (`--port`, `--data`, `--admin-socket`). |
| `mo diff [OLD] NEW` | Report capability authority, public/wire API, saved-state schema, and behavioral contract deltas against Git `HEAD` or `OLD`. |
| `mo fetch` | Fetch a verified, mode-`0600` state or failure export (`.moex`) over `--admin-socket`. |
| `mo push FILE` | Push a typed live upgrade (with optional migration) to a running `service` over `--admin-socket`. |
| `mo rehearse` | Rehearse a candidate upgrade and migration offline against an exported state snapshot. |
| `mo history DIR` | Inspect a stopped service data directory (snapshots, log tail, retained failures). |
| `mo replay FILE` | Replay a captured failure offline with the exact inputs and budgets, and verify a candidate `--fix`. |
| `mo query FILE FN` | Run a pure Mo query function against live service state, an exported `.moex` artifact, or a captured failure. |
| `mo guide` | Print the embedded build guide, or the full Language Reference with `--reference`. |

---

## What's in this repository

- **[`crates/`](Cargo.toml)** — 18 pure safe Rust crates (`mo-def`, `mo-base`, `mo-syntax`, `mo-check`, `mo-lower`, `mo-core`, `mo-codegen`, `mo-bytecode`, `mo-heap`, `mo-prim`, `mo-host`, `mo-vm`, `mo-test`, `mo-service`, `mo-upgrade`, `mo-cli`, `mo-fe`, `mo-rt`).
- **[`lib/`](docs/design/library.md)** — Standard library written in Mo (embedded in the `mo` binary), including `Http`, `Json`, `Sqlite`, `Command`, `Crypto`, `Date`, `Unicode16`, `Api`, and the 12-module **[Arvo](docs/arvo.md)** web framework (`lib/arvo/`).
- **[`examples/`](examples/README.md)** — Twelve core sample programs plus the full-stack **[Arvo + Inertia v2 starter](examples/arvo-starter/README.md)**.
- **[`tools/`](tools/README.md)** — Ten real CLI programs written in Mo, including `mox` (HTTP client), `moloc` (polyglot line counter), `moq` (JSON/CSV/SQLite slicer), `gh-sync`, `dev-pulse`, `work-etl`, `mo-harness` (confined coding-agent harness), `linkcheck`, `notioncopy`, and `tally`.
- **[`services/`](services/README.md)** — Six services written in Mo: `mo-gateway` (LLM proxy & token budgeter), `mo-pocket` (backend-in-a-box), `mo-pulse` (error tracker & uptime monitor), `mo-pipe` (scheduled ETL daemon), `mo-vcr` (recording HTTPS chaos proxy), and `task-queue` (durable MCP service).
- **[`editors/`](editors/README.md)** — Syntax highlighting, queries, and format-on-save extensions for **Zed** (Tree-sitter) and **Cursor / VS Code** (TextMate).
- **[`formal/`](formal/README.md)** — Mo Core formalized in Lean 4 with mechanized type-safety, saved-state, and replay-verdict proofs, plus the `mocore` differential-testing oracle.
- **[`grammar/`](grammar/README.md)** — Executable Bison LR(1) grammar verifying syntax unambiguity across the reference, standard library, examples, tools, and services.
- **[`models/`](models/README.md)** — TLA+ models of immutable state export, failure capture, and atomic live upgrade swaps.
- **[`learnability/`](learnability/README.md)** — Archived empirical evaluations measuring how well fresh AI models write and operate Mo from the reference alone.

---

## Documentation

| Document | Audience | What's inside |
| --- | --- | --- |
| **[Why Mo](docs/why-mo.md)** | Everyone | Motivation, language influences, and the reasoning behind every feature. |
| **[Mo Language Reference](docs/reference/README.md)** | Humans & agents | Complete, self-contained specification of Mo syntax, types, capabilities, specs, and core library (`mo guide --reference`). |
| **[Agent & Build Guide](docs/agent-guide.md)** | Agents & CLI users | Practical workflow for writing, testing, serving, diffing, replaying, and upgrading Mo programs (`mo guide`). |
| **[Arvo Web Framework](docs/arvo.md)** | Web developers | Building full-stack single-binary web apps with Mo, `api`, SQLite, and Inertia v2. |
| **[Architecture](docs/design/architecture.md)** | Contributors & systems readers | Compiler pipeline, 18-crate boundaries, Perceus memory model, VM, and test strategy. |
| **[Sandbox Properties](docs/design/sandbox-properties.md)** | Security readers | Threat model, numbered guarantees (P1–P11), enforcement points, verification evidence, and gaps. |
| **[Bytecode Specification](docs/design/bytecode.md)** | VM readers | `MOBC` v0.3 binary format, instruction set, ownership verifier, and text format. |
| **[Library Map](docs/design/library.md)** | Systems readers | Exact mapping of every core function and operator across instructions, primitives, host ops, lowering, and `lib/*.mo`. |
| **[Roadmap](docs/ROADMAP.md)** | Everyone | What has shipped and where Mo is heading next. |

Additional technical design notes live in [`docs/design/`](docs/design/architecture.md): [services](docs/design/services.md), [saved state](docs/design/saved-state.md), [introspection & replay](docs/design/introspection.md), [live upgrades](docs/design/upgrades.md), and [outside-world capabilities](docs/design/outside-world.md).

---

## Running checks

```sh
bash ci/check.sh rust     # fmt, clippy, workspace & doc tests, rustdoc, safety & linkage audit, release build
bash ci/check.sh          # full suite: rust, drivers, bench, lean, static, models (skips missing optional tools)
```

---

## License

Licensed under either of [Apache License, Version 2.0](LICENSE-APACHE) or [MIT license](LICENSE-MIT), at your option.

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion in Mo by you, as defined in the Apache-2.0 license, shall be dual licensed as above, without any additional terms or conditions.

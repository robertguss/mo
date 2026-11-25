# Roadmap

Mo evolves through concrete engineering milestones backed by real programs and verification, not calendar dates.

## Shipped (`v0.3.0`, pure safe Rust)

Everything below is implemented and tested on `main`:

- **Core language & front end:** Unambiguous Bison-verified grammar (`grammar/mo.y`), recursive-descent parser with single-cause error recovery, 100-column comment-preserving formatter (`mo format`, `--stdin`), whole-program type/ability/capability/exhaustiveness checker (`mo check`, `mo fix`), monomorphizing lowering to Mo Core, and `mo.toml` local path package resolution.
- **Formal core & verification:** Mo Core formalized and proven type-safe in Lean 4 (`formal/`), differential tests comparing the pure-Rust compiler and VM against Lean, and TLA+ protocol models (`models/`).
- **Perceus codegen, verified bytecode & pure-Rust VM:** Whole-program borrow inference, reference counting, opportunistic in-place reuse (`RESET`/`REUSE`/`SET_FIELD`), `MOBC` v0.3 binary format and load-time verifier, portable `.bc` execution (`mo emit`, `mo run FILE.bc`), `mo install` launcher packaging, and a 17-crate `#![forbid(unsafe_code)]` Rust workspace with zero OpenSSL and zero system `libsqlite3` dynamic linkage.
- **Executable specifications & diffs:** Code-first `promise` blocks with automatic non-vacuity checking, `examples`, `test`, deterministic input generation and shrinking, automatic `service`/`actor` crash-recovery testing, opt-in mutation auditing (`mo test --mutants`), and structural/behavioral diffing (`mo diff`).
- **Outside-world capabilities & standard library:** Directory-confined `Files`/`ReadFiles`/`Files.File` (random-access binary I/O), `Network` (TCP + TLS 1.2+ via `rustls`, `allow_hosts`), `Http` client, `Sqlite` (WAL mode, authorizer guards, typed `Sqlite.decode[T]`, `Sqlite.transaction`), `Command` (shell-free, program/folder allowlists, Linux Landlock + seccomp confinement), `SecureRandom`, pure-Rust `Crypto`, `Date`, `Unicode16`, and compiler-derived `Json` ability.
- **Concurrent `api` servers, `actor`s & Arvo web framework:** Multi-worker HTTP/1.1 `api ... end` servers (`scope`, confined `static` mounts with `ETag`/`304`, type-driven `auth`, exhaustive `status`, automatic OpenAPI 3.1 + `/docs`, background `job`s, `--watch` reload, zero-socket `ApiTest.send`), composable in-process `actor Name ... end` capabilities, and the 12-module pure-Mo [Arvo](arvo.md) web framework (`lib/arvo/`) with [`examples/arvo-starter/`](../examples/arvo-starter/README.md).
- **Durable MCP `service`s, introspection & live upgrades:** SQLite snapshot + recovery-log persistence, crash supervision, mode-`0600` `--admin-socket` operator views, immutable `mo fetch` exports, offline `mo history`/`mo replay`/`mo query`, offline `mo rehearse`, and atomic typed live upgrades via `mo push`.
- **Real programs & editor support:** Twelve core samples (`examples/`), ten CLI tools (`tools/`, including `mox`, `moloc`, `moq`, and `mo-harness`), six services (`services/`), and Zed + Cursor/VS Code editor extensions (`editors/`).

## Next

Surface syntax additions are deliberately paused unless earned by friction in real daily use. Focus areas ahead:

- **Daily-driver stability & ergonomics:** Hardening `mo serve`, `Arvo`, SQLite tooling, and standalone CLI tools through real production workloads.
- **Performance:** Profiling VM dispatch, collection reuse, and I/O hot paths; exploring ahead-of-time or JIT native code generation once bytecode semantics stabilize.
- **Packages & ecosystem (toward `1.0`):** Expanding `mo.toml` beyond local path dependencies to versioned package distribution, lockfiles, and community sharing, alongside user-defined abilities when needed.
- **Platform confinement:** Extending OS-level child confinement ergonomics across Linux and macOS.
- **Ongoing tracks:** Maintaining Lean 4 Core proofs, TLA+ models, fuzzing, security documentation, and up-to-date embedded docs (`mo guide`).

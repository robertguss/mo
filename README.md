# Mo

**An open-source language and runtime for living software.** Humans state the
intent, agents write the code, and agents can inspect and diagnose running services.
The development toolchain also supports typed live upgrades and state migration.

> **Status: v0.2.0.** The toolchain builds and
> runs services. The [Phase 4 diagnostic evaluation](docs/status/phase4-run.md)
> passed its gate with 14 of 16 sessions correct, including two recorded misses.
> Typed live upgrades shipped in v0.2.0, which closes Phase 5: its drill, pause
> series and 4/4 agent evaluation passed on one frozen build. See the
> [upgrade run record](docs/status/phase5-run.md).
> Contributions are not yet open.

## The idea

Agents now write a growing share of software, but languages and runtimes are
still built for human authors and human operators. Mo is designed for agents
first:

- **Intent lives in the language.** People state what a program must do, as
  promises and example tables. The toolchain checks programs and runs their
  tests. The approval workflow for protected intent is planned for Phase 6.
- **A runtime agents can operate.** Agents can look inside a running Mo
  service through authenticated, bounded operator views. Pure queries run
  offline over exported state. Failure replay reconstructs a recorded case
  with the original program and runtime build; it does not reproduce the
  live heap or mailbox. A verified fix can still be wrong: independent tests
  rejected two such fixes in the diagnostic evaluation.
- **Upgrades while running.** The development toolchain checks candidate programs
  and typed migrations, rehearses them offline, and activates them at a service
  message boundary. `mo push` uses a separate upgrade credential and a loopback
  connection; remote operators use a tunnel. The drill, the pause series and the
  agent evaluation passed on one frozen build. This functionality ships in v0.2.0.

Mo draws on Elixir and the BEAM (processes, supervision, hot code loading),
Rust (safety plus control over memory and performance), Koka (effects and
in-place updates) and Elm (helpful error messages).

## What's here

- [The Mo Language Reference v0.1](docs/reference/README.md): core Mo, the
  language later versions build on, written to be read in full by people and
  agents.
- [Architecture v0.1](docs/design/architecture.md) and
  [the bytecode specification](docs/design/bytecode.md): how the toolchain and
  virtual machine are built.
- [Mo Core](formal/README.md): the small language every feature lowers to,
  defined in Lean, with its type safety proven, independent fake capabilities
  for specs, xoshiro256** Random, folder entries, trusted Unicode16 interfaces,
  D236 file-kind checks and Float layout, D237 persistent folders and explicit
  folder creation, and the [Core text 0.8 `mocore` oracle](formal/TEXT.md), shared with C.
- [Twelve sample programs](examples/), and the
  [executable grammar](grammar/README.md) every example parses with.
- [The learnability test](learnability/): fresh models from two families
  writing Mo from the reference alone, and how they did.

## Building

Mo is implemented in pure safe Rust (`#![forbid(unsafe_code)]`, zero OpenSSL, zero system
`libsqlite3`). Building requires a Rust toolchain (`cargo`, edition 2024):

```sh
cargo build --release --bin mo   # build target/release/mo
bash ci/check.sh                 # every check, run locally
```

## Install from source

Check out the repository and build the standalone `mo` binary in its permanent location:

```sh
git clone https://github.com/robertguss/mo
cd mo
cargo build --release --bin mo
./target/release/mo --version
./target/release/mo guide
```

Keep the tree where it was built: the executable contains its `lib/` path.
A local rebuild has its own identity; replay of an existing failure needs the
exact executable that recorded it. For a service installation and keeping
those builds, see the [task-queue runbook](docs/ops/task-queue.md).

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md). Phase 4 closed with v0.1.0 and Phase 5,
typed live upgrades, closed with v0.2.0: its 1,000-kill drill, pause series and
agent evaluation (4/4) passed on one frozen build. [v0.2 preparation notes](docs/releases/v0.2-preparation.md)
describe the implemented behavior and unresolved limits.

## Background

The research behind Mo is archived in
[robertguss/mo-v2](https://github.com/robertguss/mo-v2). It includes 13
experiments and Lean proofs of Mo's memory-reuse rules.

## License

Licensed under either of [Apache License, Version 2.0](LICENSE-APACHE) or
[MIT license](LICENSE-MIT), at your option.

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in Mo by you, as defined in the Apache-2.0 license, shall be dual
licensed as above, without any additional terms or conditions.

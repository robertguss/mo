# Mo

**An open-source language and runtime for living software.** Humans state the
intent, agents write the code, and agents can inspect and diagnose running services.
Typed live upgrades are planned for a later phase.

> **Status: v0.1.0.** The toolchain builds and
> runs services. The [Phase 4 diagnostic evaluation](docs/status/phase4-run.md)
> passed its gate with 14 of 16 sessions correct, including two recorded misses.
> This first release has a narrow scope; typed live upgrades are planned.
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
- **Living software, planned.** Phase 5 targets typed live upgrades and state
  migration. The current toolchain does not provide them.

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

Mo is implemented in C. Building requires a C17 compiler, POSIX, `make` and
Python 3 (to embed the version-matched reference and guide):

```sh
make              # build build/<compiler>/debug/mo
bash ci/check.sh  # every check, run locally
```

See [docs/design/implementation.md](docs/design/implementation.md) for the
rules the implementation follows and how it is checked.

## Install v0.1.0 from source

Check out the release tag and build it in its permanent location. Its build inputs
match the evaluated candidate recorded in [the freeze](learnability/phase4/freeze.json):

```sh
git clone https://github.com/robertguss/mo mo-v0.1
cd mo-v0.1
git checkout --detach v0.1.0
make CC=gcc build/gcc/debug/mo
./build/gcc/debug/mo --version
./build/gcc/debug/mo guide
```

Keep the tree where it was built: the executable contains its `lib/` path.
A local rebuild has its own identity; replay of an existing failure needs the
exact executable that recorded it. For a service installation and keeping
those builds, see the [task-queue runbook](docs/ops/task-queue.md).

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md). Phase 4 closes with v0.1.0;
Phase 5 targets typed live upgrades.

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

# Mo

**An open-source language and runtime for living software.** Humans state the
intent, agents write the code, and agents can see inside the running system,
diagnose it, and evolve it safely while it runs.

> **Status: pre-release, design phase.** Mo isn't usable yet, and nothing here
> is stable. The repository is public so the work is visible, but it isn't
> accepting contributions yet.

## The idea

Agents now write a growing share of software, but languages and runtimes are
still built for human authors and human operators. Mo is designed for agents
first:

- **Intent lives in the language.** People state what a program must do, as
  promises and example tables, and approve it. Agents write the
  implementation, and the compiler won't let them quietly weaken an approved
  promise.
- **A runtime agents can operate.** Agents can look inside a running Mo
  system: its processes, state, memory and history. They can run safe live
  queries and replay failures exactly.
- **Living software.** Running services evolve without restarts. State
  migrations are type-checked, versions are checked for compatibility, and
  upgrades are checked against the real state before anything changes.

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
  D236 file-kind checks and Float layout, and the
  [Core text 0.6 `mocore` oracle](formal/TEXT.md), shared with C.
- [Twelve sample programs](examples/), and the
  [executable grammar](grammar/README.md) every example parses with.
- [The learnability test](learnability/): fresh models from two families
  writing Mo from the reference alone, and how they did.

## Building

Mo is implemented in C. With a C compiler and `make`:

```sh
make              # build build/<compiler>/debug/mo
bash ci/check.sh  # every check CI runs
```

See [docs/design/implementation.md](docs/design/implementation.md) for the
rules the implementation follows and how it is checked.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md). The first public release, v0.1, is
planned for when agents can build services in Mo and see inside them while
they run.

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

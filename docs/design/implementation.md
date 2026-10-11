# How Mo is implemented

Mo's compiler and runtime are written in C (decision D207, 11 Oct 2026). C was
chosen for fast compiles and quick iteration while the language and runtime are
being proven, and for direct control of memory, since Mo's VM is built around
its memory model: counting holders and reusing memory in place. A later port,
for example to Rust, stays open. The Lean model, the golden corpus and the
tests that check the VM against the Lean model are the lasting investment: they
carry over to any port.

## The five conditions

C has no memory safety, and Mo's capability security is only as strong as the
VM's integrity: a memory bug in the VM could let a program reach something it
wasn't handed. These five conditions make up for that. They are enforced by
checks, not by good intentions; `ci/check.sh` runs every check.

1. **Sanitizers in every test run.** Every test also runs under
   AddressSanitizer and UndefinedBehaviorSanitizer, and concurrent code under
   ThreadSanitizer, so memory bugs fail loudly where they happen.
2. **Fuzzing in CI.** The parser, the bytecode loader, the VM and everything
   that reads outside input get fuzz targets in `fuzz/`, with seed inputs in
   `fuzz/corpus/<target>/`. CI runs every target on every commit.
3. **Tiger Style.** Assertions everywhere (`MO_ASSERT` is never compiled out),
   explicit limits, arenas instead of scattered `malloc` and `free`, every
   return value checked (`MO_MUST_USE`), and warnings treated as errors,
   including `-Wconversion`: like Mo itself, the implementation never converts
   between kinds of numbers silently.
4. **A small trusted core.** Only the VM, primitives and system calls are C.
   The standard library above them, including JSON, HTTP routing and MCP, is
   written in Mo, which is memory-safe by construction.
5. **Bytecode verified when it's loaded,** as WebAssembly does, because upgrade
   packages will arrive over the network in Phase 5.

## Building and checking

```sh
make                # build build/<compiler>/debug/mo
make test           # unit tests
make sanitize       # the tests under AddressSanitizer and UndefinedBehaviorSanitizer
make fuzz-smoke     # every fuzz target for 20 seconds (needs clang with libFuzzer)
make format         # format the C sources with clang-format
bash ci/check.sh    # everything CI runs: run it before every push
```

Choose a compiler with `CC=gcc` or `CC=clang`. Each compiler builds into its
own directory, and only changed files recompile.

## CI

CI runs in a dedicated Amp orb (decision D208): GitHub sends every push and
pull request to the CI orb, which runs the commit's `ci/check.sh` and reports a
`mo-ci` commit status. See `ci/orb/README.md`.

## Rust-era plans, replaced

| Planned with Rust | With C |
| --- | --- |
| Miri on the raw-memory core | AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer on every test run |
| Kani proofs | CBMC, the engine underneath Kani, on small C functions |
| Cranelift for machine code (Phase 7) | A C code generator such as MIR, or copy-and-patch as CPython does |
| rustls | OpenSSL or BoringSSL, through the small trusted core |
| The Rust MCP SDK | MCP written in Mo |

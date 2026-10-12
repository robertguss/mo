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
wasn't handed. These five conditions make up for that. Checks enforce them
wherever there is code to check, not good intentions; `ci/check.sh` runs every
check.

1. **Sanitizers in every test run.** Every test also runs under
   AddressSanitizer and UndefinedBehaviorSanitizer, so memory bugs fail loudly
   where they happen. ThreadSanitizer joins them in Phase 3, when concurrent
   code arrives; there is none to check yet.
2. **Fuzzing in CI.** The parser, the bytecode loader, the VM and everything
   that reads outside input get fuzz targets in `fuzz/`, with seed inputs in
   `fuzz/corpus/<target>/`. `ci/check.sh` runs every target, so every CI run fuzzes them all.
3. **Tiger Style.** Assertions everywhere (`MO_ASSERT` is never compiled out),
   explicit limits, arenas instead of scattered `malloc` and `free`, every
   return value checked, and warnings treated as errors, including
   `-Wconversion`: like Mo itself, the implementation never converts between
   kinds of numbers silently. Two checks cover return values. `MO_MUST_USE` on
   one of Mo's own functions makes ignoring its result a compile error.
   `make tidy` runs clang-tidy's `cert-err33-c` check, which does the same for
   the standard-library functions on CERT rule ERR33-C's list, such as
   `fflush`, `fwrite`, `malloc` and `strtol`. That list leaves out `printf`,
   `puts` and `putchar`, so their results are checked by convention only. A
   `(void)` cast discards a result on purpose.
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
make tidy           # standard-library results are used (needs clang-tidy)
make grammar        # Mo code parses with the executable grammar (needs bison)
make formal         # Mo Core in Lean builds and every proof checks (needs elan)
bash ci/check.sh    # everything CI runs: run it before every push
```

Choose a compiler with `CC=gcc` or `CC=clang`. Each compiler builds into its
own directory, and only changed files recompile; changing the Makefile
recompiles everything. `make sanitize` stops at the first error by default;
set `ASAN_OPTIONS` or `UBSAN_OPTIONS` in your environment to use your own
options instead.

## The executable grammar

`grammar/` holds Mo's grammar as a Bison file, with the lexer and the checks a
grammar can't express. It is an instrument, not the compiler: it checks that
the reference's examples, the programs in `examples/` and the learnability
test's answers parse, and it proves the grammar unambiguous, because Bison
refuses a grammar with any conflict. Phase 2's parser is hand-written, for
better error messages, and is tested against this grammar. See
`grammar/README.md`.

## CI

CI runs in a dedicated Amp orb, on request (decisions D208 and D214). An
implementation thread pushes its branch, opens a pull request and sends it to
the CI orb, which runs that commit's `ci/check.sh` in a clean environment,
comments the result on the pull request, and replies. Nothing runs on GitHub.
See `ci/orb/README.md`.

## Rust-era plans, replaced

| Planned with Rust | With C |
| --- | --- |
| Miri on the raw-memory core | AddressSanitizer and UndefinedBehaviorSanitizer on every test run, and ThreadSanitizer once there is concurrent code (Phase 3) |
| Kani proofs | CBMC, the engine underneath Kani, on small C functions |
| Cranelift for machine code (Phase 7) | A C code generator such as MIR, or copy-and-patch as CPython does |
| rustls | OpenSSL or BoringSSL, through the small trusted core |
| The Rust MCP SDK | MCP written in Mo |

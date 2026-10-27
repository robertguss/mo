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
check. Nightly runs `gcc clang sanitize drivers static lean` on main; fuzz and
CBMC run by hand. The fast groups run before each pull request.

1. **Sanitizers in every test run.** Every test also runs under
   AddressSanitizer and UndefinedBehaviorSanitizer (`bash ci/check.sh sanitize`,
   run by hand), so memory bugs fail loudly where they happen. ThreadSanitizer joins them in Phase 3, when concurrent
   code arrives; there is none to check yet.
2. **Fuzzing.** The parser, the bytecode loader, the VM and everything
   that reads outside input get fuzz targets in `fuzz/`, with seed inputs in
   `fuzz/corpus/<target>/`. `bash ci/check.sh fuzz` runs every target briefly;
   `ci/orb/long-fuzz.sh` fuzzes each one for five minutes when run by hand.
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

`sandbox-properties.md` states, property by property, what these conditions
guarantee about a loaded program, where each guarantee is enforced and what
evidence backs it, and it lists the gaps.

## Building and checking

```sh
make                # build build/<compiler>/debug/mo
make test           # unit tests
make sanitize       # the tests under AddressSanitizer and UndefinedBehaviorSanitizer
make fuzz-smoke     # every fuzz target for 5 seconds (needs clang with libFuzzer; Makefile FUZZ_SECONDS)
make format         # format the C sources with clang-format
make deps           # each module includes only what the architecture allows
make library-map    # docs/design/library.md and the tables of primitives agree
make tidy           # standard-library results are used (needs clang-tidy)
make grammar        # Mo code parses with the executable grammar (needs bison)
make formal         # Mo Core in Lean builds and every proof checks (needs elan)
bash ci/check.sh    # every check, including the slow ones
```

Choose a compiler with `CC=gcc` or `CC=clang`. Each compiler builds into its
own directory, and only changed files recompile; changing the Makefile, or
adding or removing a C file in `src/`, recompiles everything. `make sanitize`
stops at the first error by default; set `ASAN_OPTIONS` or `UBSAN_OPTIONS` in
your environment to use your own options instead.

The C sources are in `src/`, with a directory for each module of the
architecture (section 2), and the Makefile builds every C file in them.
Headers are included by their path in `src/`, such as `"base/base.h"`.
`make deps` checks that each module includes only the headers that section
allows it. The rules are a table at the top of `ci/deps.sh`.

## The executable grammar

`grammar/` holds Mo's grammar as a Bison file, with the lexer and the checks a
grammar can't express. It is an instrument, not the compiler: it checks that
the reference's examples, the programs in `examples/` and the learnability
test's answers parse, and it proves the grammar unambiguous, because Bison
refuses a grammar with any conflict. Phase 2's parser is hand-written, for
better error messages, and is tested against this grammar. See
`grammar/README.md`.

## Checks and merging

Before a pull request, `bash ci/check.sh gcc` builds and
runs the unit tests. Nightly runs `gcc clang sanitize drivers static lean`;
fuzz and CBMC run by hand. A pull request merges when Robert says so. The merge
rule is in AGENTS.md "Checks and merging", and the evidence a PR records is in
`ci/orb/README.md`.

## Rust-era plans, replaced

| Planned with Rust | With C |
| --- | --- |
| Miri on the raw-memory core | AddressSanitizer and UndefinedBehaviorSanitizer on every test run, and ThreadSanitizer once there is concurrent code (Phase 3) |
| Kani proofs | CBMC, the engine underneath Kani, on small C functions |
| Cranelift for machine code (Phase 7) | A C code generator such as MIR, or copy-and-patch as CPython does |
| rustls | OpenSSL or BoringSSL, through the small trusted core |
| The Rust MCP SDK | MCP written in Mo |

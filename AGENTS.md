# AGENTS.md

Guidance for coding agents (Claude Code, Codex, Cursor and others) working in this repository.
`CLAUDE.md` imports this file.

## What this is

Mo is a pre-release language and runtime for "living software", designed for agents first. The
toolchain is one pure-Rust program, `mo` (`check`, `run`, `test`, `format`, `fix`, `emit`,
`serve`, `fetch`, `push`, `replay`, `query`, `rehearse`, `history`, `diff`, `install`, `guide`),
built from an 18-crate Cargo workspace (`#![forbid(unsafe_code)]`, zero OpenSSL, zero system
`libsqlite3`) that compiles a whole program from source to bytecode and runs it on its own VM.
Commit and PR titles say what the change does. Older commits and documents cite decisions as `D2xx`
and tasks as `MO-NN`; neither numbering continues, and a decision is written as the rule itself in
the document it belongs to.

The documents that define correct behaviour, in order of authority:

- `docs/reference/README.md`: the Mo Language Reference v0.1. It is embedded into the binary
  (`mo guide --reference`) by `crates/mo-cli/build.rs`, as is `docs/agent-guide.md`.
- `docs/design/architecture.md`: how the toolchain is built (sections 2, module boundaries, and
  11, testing strategy, are the ones to know). `docs/design/bytecode.md` is the bytecode spec.
- `docs/design/library.md`: where each library function is implemented.
- `docs/status/sample-blockers.md`: sample status (currently clear).

## Commands

Requires a Rust toolchain (`cargo`, edition 2024, `rustfmt`, `clippy`) and Python 3. Full CI also
uses `bison` (for `grammar/mo.y`), Lean via `elan` (`/opt/elan` is where `ci/check.sh` looks first;
the Lean version is named in `formal/lean-toolchain`), and Java + TLC (for `models/`).

```sh
cargo build --release --bin mo                 # build target/release/mo
cargo test --workspace --all-targets           # run every Rust unit & integration test
cargo clippy --workspace --all-targets --all-features -- -D warnings
cargo fmt --all -- --check
make -C grammar grammar                        # executable Bison grammar over grammar/tests, examples/, services/, the reference
sh ci/library_map.sh                           # docs/design/library.md vs crates/mo-def/defs/prims.def and hostops.def
bash ci/check.sh [group…]                      # every check; groups: rust drivers bench lean static models
ci/wait-for-results FILE [TIMEOUT]             # wait for a detached ci/check.sh run (see Waiting on checks)
```

- Warnings are errors (`cargo clippy --workspace --all-targets --all-features -- -D warnings`,
  `RUSTDOCFLAGS="-D warnings" cargo doc --workspace --no-deps`), and every crate uses
  `#![forbid(unsafe_code)]` with `[lints] workspace = true`.
- Locally, `ci/check.sh` skips checks whose optional tool is missing and says so in its summary;
  with `CI=1` a missing tool fails. A skip is not a pass.
- The `drivers` group runs Python black-box checks against `target/release/mo`
  (`ci/check_cli.py` and its `ci/cli_*.py` topic modules for CLI text/JSON goldens,
  `ci/check_golden.py`, `ci/check_errors.py`, `ci/check_format.py`, …); the `lean` group builds
  Mo Core in Lean and compares the pure-Rust toolchain with it; the `bench` group runs benchmark v0,
  the three Mo-written tools and the learnability calculators. CLI behaviour changes usually need an
  edit to the `ci/cli_*.py` module for that topic, to `check()` in `ci/check_cli.py`, or to the
  goldens under `tests/cli/`.

## Checks and merging

Before opening a PR, run `bash ci/check.sh rust` (format, clippy, tests, docs, safety/linkage
audit, release build) and put its last line in the PR body. Say which other groups you ran, if any;
do not run groups to be thorough, the nightly does that. A skip for a missing tool is not a pass;
`CI=1` makes it fail instead. The one exception: after editing `docs/agent-guide.md`,
`docs/reference/README.md` or `docs/design/library.md`, which the build embeds or checks, also run
`bash ci/check.sh drivers static` once.

A PR that changes a Mo program under `lib/`, `examples/` or `services/task-queue/` also runs plain
`target/release/mo test` on each changed program and puts its last line in the PR body. For the task
queue the program is `services/task-queue/queue-tests.mo`.

The `nightly` GitHub Actions workflow checks `main` daily at 07:17 UTC and on manual dispatch,
never on pull requests or pushes. It runs `rust`, `static`, `drivers`, `bench`, and `lean` with
`CI=1` in parallel jobs, plus a 1,000-program Rust-vs-Lean Core differential check. A red nightly
is the next PR, before any feature.

`main` requires no status check (only force-pushes are blocked). A PR gets one review and merges
when Robert says so; he lets agents merge docs-only PRs (Markdown only, other than the three
files above) without review. PRs are squash-merged. Claims about what Mo guarantees get the
Working Agreement's full treatment (predictions written first, an independent check, failures
kept); ordinary building gets this section and nothing more.

### Waiting on checks

A run of `ci/check.sh` takes longer than one Bash tool call allows. Start it detached with its
output in a results file, then wait for it in the foreground, in the same turn. Never end the turn
while checks run.

```sh
mkdir -p build
rm -f build/check-results.txt
nohup timeout --kill-after=30 3600 bash ci/check.sh rust >build/check-results.txt 2>&1 &
```

`rm -f` keeps an earlier run's marker out of this run's file. `timeout` ends a hung run, which then
reads as failed, so raise its 3600 seconds for a longer run such as `cbmc`. Then, in a separate
call:

```sh
ci/wait-for-results build/check-results.txt
```

Repeat that call until it exits 0 or 1. Each call waits up to 540 seconds, or the `TIMEOUT` you
pass. `TIMEOUT` 0 checks once. Give the tool call a longer timeout than that, or it dies first.

| Exit | Meaning | What to do |
| --- | --- | --- |
| 0 | The checks passed. | Report the result. A pass with skips still prints what was skipped. |
| 1 | The checks failed, the run stopped without a marker, or a minute passed with no started line. | Read what it printed and the results file. |
| 66 | The file is missing or not a regular file. | Start `ci/check.sh` first. |
| 75 | The run is still going. | Call `ci/wait-for-results` again. |

A run ends with `check.sh finished: pass` or `check.sh finished: fail`, and only the last line of
the file counts. `2>&1` matters: the `failed:` lines naming each failed check go to stderr.

## Architecture

Pipeline: source → parser (`crates/mo-syntax`) → names, types and rule checks (`crates/mo-check`) →
lowering to Mo Core and monomorphization (`crates/mo-lower`) → Core checker (`crates/mo-core`) →
bytecode compiler with reference counting, borrowing and in-place reuse (`crates/mo-codegen`) →
loader and verifier (`crates/mo-bytecode`) → VM (`crates/mo-vm`). The VM never sees source, syntax
trees or Core.

The workspace crates live under `crates/` (`mo-def`, `mo-base`, `mo-core`, `mo-syntax`, `mo-check`,
`mo-lower`, `mo-codegen`, `mo-fe`, `mo-bytecode`, `mo-heap`, `mo-prim`, `mo-host`, `mo-vm`,
`mo-rt`, `mo-test`, `mo-service`, `mo-upgrade`, `mo-cli`).

Four shared contracts live in `crates/mo-def/defs/` (and are parsed at build time by `crates/mo-def/build.rs`); changing one means changing every side that reads it:

- **Mo Core**: `formal/` (Lean, the reference evaluator), `crates/mo-def/defs/{core.h,ops.def}`,
  and `crates/mo-core`, exchanged as text (`formal/TEXT.md`, `formal/TRANSLATION.md`).
- **Bytecode**: `docs/design/bytecode.md`, `crates/mo-def/defs/opcodes.def`, and `crates/mo-bytecode`.
- **Primitives**: `crates/mo-def/defs/prims.def`. Every line needs a row in `docs/design/library.md`
  (`sh ci/library_map.sh`).
- **Capability operations**: `crates/mo-def/defs/hostops.def` and `crates/mo-host`. All
  outside-world effects go through capabilities, which have real and fake implementations.

## Rust rules

- `#![forbid(unsafe_code)]` and `[lints] workspace = true` in every crate and binary entry point.
- Zero OpenSSL (`libssl`/`libcrypto`) and zero system `libsqlite3` dynamic linkage.
- Every input has a named limit in `crates/mo-def/defs/limits.h`. Compiler-input limits are
  diagnostics; runtime limits (stack, call depth, heap) are bug reports.
- Keep `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets --all-features -- -D warnings`,
  `cargo test --workspace --all-targets`, `cargo test --workspace --doc`, and
  `RUSTDOCFLAGS="-D warnings" cargo doc --workspace --no-deps` clean.

## Tests

- Unit and integration tests live in `crates/*/src/*test*.rs` and `crates/*/tests/*.rs`.
- Checker diagnostics are pinned by `tests/check/diagnostics/eNNNN` inputs with adjacent `.diag`
  expectations (format in that folder's `README.md`). Nothing regenerates them; edit by hand.
- Every code in `crates/mo-def/defs/codes.h` needs a case in `tests/errors/` (exact `mo check`
  message, see its `README.md`) or a line in `tests/errors/unit.tsv`; `ci/check_errors.py` fails
  otherwise.
- `tests/golden/` is the golden corpus: whole programs with their exact stdout, stderr and exit
  status (format in `ci/check_golden.py`). Write the expected output by hand from the reference; a
  wrong-looking result is a bug to fix or list in `docs/status/golden-gaps.md`, never a new
  expectation.
- `docs/agent-guide.md` (`mo guide`) tells agents what this build does. A change to what a `mo`
  command does, or to which forms are unfinished, updates the guide in the same PR;
  `guide_claims` in `ci/cli_guide.py` runs the guide's program and checks each claim it makes.
- `grammar/mo.y` is an instrument, not the compiler: the real parser is hand-written in
  `crates/mo-syntax/`. Bison refuses any conflict, which keeps the grammar unambiguous.

## Workflow

The PR template asks what changes, which tests prove it, and what `bash ci/check.sh rust` said.
A one-line fix keeps each to a sentence; it still gets a test.

`docs/status/agent-mistakes.md` is the ledger of mistake classes agents have repeated here. Read it
before C work; it is reviewed at phase boundaries, not bumped in every PR.

`docs/solutions/` holds learnings from past work: the problem, why it happened and what to do,
with `module` and `tags` in each file's frontmatter. Search it before working in an area it covers.

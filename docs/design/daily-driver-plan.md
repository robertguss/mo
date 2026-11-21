# Daily-Driver Readiness Plan

Status: Active. Follows the completion of the [Post-Phase 7 direction](post-phase7-direction.md)
(`#385`–`#399`).

## Goal

Make Mo ready for daily personal and professional use on Robert's machines (Apple Silicon macOS
and x86_64 Linux, edited in Zed and Cursor), starting with exploratory and tactical data
wrangling against real external APIs (such as Populi and Canvas LMS), local SQLite staging and
reconciliation, and standalone CLI tools on `$PATH`.

## Governing Rule: Freeze Speculative Surface Features

Mo's core language surface (`struct`, `enum`, abilities including `Json`, `Option`/`Result` with
`try` and `or`, `Decimal`, `Sqlite`, `Command`, `actor`, `api`, and code-first `promise`s) is
frozen against speculative additions. From this point forward:

1. No new syntax or standard-library surface is added "for completeness."
2. We fix host boundaries, security/concurrency hardening, workstation installation, editor
   ergonomics, and the concrete gaps required to run real API and data-wrangling spikes.
3. Every subsequent library or language change must be earned by friction encountered in real
   daily work.

## Where Mo Fits Alongside Rust (Keel Data Stack)

- **Rust (`fetchkeel`, `dapkeel`, `loadkeel`, `statkeel`, `runkeel`)** handles hardened,
  high-throughput batch extraction, Parquet/COPY replication, and static SQL lineage.
- **Mo** handles high-iteration exploratory and tactical data work: API reconnaissance, schema
  discovery with `Json.parse` and field-ignoring `Json.decode[T]`, exact `Decimal` calculations,
  paginated extraction into local SQLite databases for ad-hoc SQL joins and cross-system audits,
  and rapid promotion of a spike into a tested, capability-confined CLI binary.

---

## Execution Plan

### Step 1: C Hardening, Release Build & CI Coverage

Close the build, fuzzing, concurrency, SQLite, CI, and documentation gaps left by the rapid
Post-Phase 7 implementation sprint:

1. **GCC `-O2` release build fix & CI gate:** Initialize `route_path_info` in
   [`src/check/apis.c`](../../src/check/apis.c) and guard `command_error_tag` length addition in
   [`src/host/command.c`](../../src/host/command.c) so `make release CC=gcc` passes `-Werror`, and
   build `release` alongside `test` in `ci/check.sh` (`gcc` and `clang` groups).
2. **Track 3 CI & grammar coverage:** Wire all programs in `tools/` and `services/mo-*/` into
   `make grammar` (`GRAMMAR_CHECK`) and a `drivers` check running `mo test` on every tool and
   service, remove the orphaned `mo-checks-fail` target in `Makefile` that broke `drivers` after
   `#390`, and remove the last deprecated `allows`/`forbids` clauses in `tools/mo-harness/`.
3. **`api` server security & concurrency hardening
   ([`src/tools/api-server.c`](../../src/tools/api-server.c)):**
   - Replace per-`recv()` socket timeouts with monotonic request read and write deadlines
     (`MO_LIMIT_API_READ_US`, `MO_LIMIT_API_WRITE_US`) returning `408 Request Timeout` so trickled
     Slowloris connections cannot starve the worker pool.
   - Bound raw chunked-transfer wire overhead, compact consumed chunk buffers, require valid
     trailing CRLF on `0`-length chunks, reject duplicate `Transfer-Encoding` headers, and reject
     ASCII control characters in request targets and header values.
   - Add `fuzz/fuzz_api_server.c` with seed inputs in `fuzz/corpus/fuzz_api_server/`.
   - Synchronize `live_actor_event` error logging with the server's `log_mutex`, and add a
     `make tsan` (`-fsanitize=thread`) target covering the multithreaded test suites.
4. **SQLite WAL mode ([`src/host/sqlite.c`](../../src/host/sqlite.c)):** Enable
   `PRAGMA journal_mode = WAL;` when opening real disk databases so concurrent `api` workers and
   local tools do not contend on rollback-journal locks.
5. **Documentation alignment:** Bring [`docs/ROADMAP.md`](../ROADMAP.md),
   [`docs/design/sandbox-properties.md`](sandbox-properties.md), and
   [`docs/reference/README.md`](../reference/README.md) section 15 into agreement with shipped
   main.

### Step 2: Workstation Installation, OS Portability & Capability Root Grants

Make `mo` and Mo-written tools installable and usable from any working directory on macOS (Apple
Silicon) and Linux without weakening capability security:

1. **Toolchain & tool installation:**
   - Add `make install` / `make uninstall` (`PREFIX ?= $(HOME)/.local`) installing a release `mo`
     binary to `$(PREFIX)/bin/mo`.
   - Add `mo install` to compile a `.mo` entry point to verified bytecode (`.bc`) and install a
     fast launcher on `$PATH` (`~/.local/bin/<name>`), avoiding `E0055` module-filename coupling
     and source-recompilation overhead.
2. **Explicit CLI capability root grants ([`src/tools/run.c`](../../src/tools/run.c)):**
   - Keep Mo's interior capability confinement strict (no `..` escapes, no ambient authority),
     while adding operator-supplied root flags on `mo run` (such as `--root DIR` and
     `--state-dir DIR`) so tools like `moloc`, `moq`, `mox`, `gh-sync`, and `mo-harness` can
     operate on explicitly granted directories and keep persistent SQLite state under
     `~/.local/share/<tool>/` instead of polluting `$PWD`.
3. **macOS & Linux workstation fixes in `Command` and TLS:**
   - Fix [`src/host/command.c`](../../src/host/command.c) on macOS: resolve `"mo"` via
     `_NSGetExecutablePath` (matching `runtime-identity.c`), replace thread-unsafe parent-process
     `fchdir()` in `fd_realpath` with `fcntl(fd, F_GETPATH, ...)`, search standard workstation
     locations (`/opt/homebrew/bin`, `~/.local/bin`) alongside `/usr/local/bin` and `/usr/bin`,
     and make non-Landlock host behavior explicit.
   - Fix [`src/host/confine.c`](../../src/host/confine.c) on Linux so seccomp allows `socketpair`
     and does not reject signals sent by grandchild helper processes to themselves (`EPERM`).
   - Add a macOS `/etc/ssl/cert.pem` root-bundle fallback in
     [`src/host/tls.c`](../../src/host/tls.c) when OpenSSL's compiled-in verify path is absent.

### Step 3: Zed & Cursor Editor Support

Give human authors syntax highlighting, block indentation, comment toggling, and `mo format`
integration in both daily editors:

1. **Cursor / VS Code (`editors/vscode/`):** TextMate grammar (`mo.tmLanguage.json`),
   `language-configuration.json`, and a one-command local installer into `~/.cursor/extensions/`.
2. **Zed (`editors/zed/`):** Tree-sitter grammar derived from [`grammar/mo.y`](../../grammar/mo.y)
   with `highlights.scm`, `brackets.scm`, `indents.scm`, and `extension.toml` ready for Zed's
   *Install Dev Extension* workflow.

### Step 4: Minimum Data-Spike Readiness (Populi, Canvas & Local SQLite)

Unblock real-world API extraction and local SQLite data wrangling before starting live spikes:

1. **Ephemeral in-memory SQLite on real `Sqlite` capabilities
   ([`src/host/sqlite.c`](../../src/host/sqlite.c), [`tools/moq/main.mo`](../../tools/moq/main.mo)):**
   Honor `":memory:"` (or `Sqlite.memory`) in real runs as an ephemeral in-memory database instead
   of creating a literal `:memory:` file on disk, and wrap `moq` bulk loads in `BEGIN`/`COMMIT`
   transactions.
2. **Rate-limit backoff (`Clock.sleep`):** Add `sleep(duration: Duration)` to `Clock` (`nanosleep`
   on real `Clock`, instant virtual-clock advance on `Clock.fixed` in tests) so Canvas and Populi
   extractors can honor `429` / `Retry-After` and rate-limit headers cleanly.
3. **Essential `Http` helpers ([`lib/http.mo`](../../lib/http.mo)):**
   - `Response.headers_named(name)` so multiple RFC 5988 `Link` pagination headers are preserved.
   - `Request.form(pairs)` for `application/x-www-form-urlencoded` OAuth2 and REST requests.
   - `Request.max_body(bytes)` so large JSON/CSV export responses can opt past the default 16 MiB
     ceiling within the VM's heap budget.

After Step 4, pause roadmap additions, install Mo on both machines, and drive further work from
live Populi and Canvas data-wrangling tasks.

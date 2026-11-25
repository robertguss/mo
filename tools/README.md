# Mo Tools

Ten standalone command-line tools written in Mo—exercising HTTPS, `Json.decode[T]`, `Sqlite`, `Files.walk`, `Unicode16`, `Decimal`, `Command` confinement, and in-process BEAM `actor`s.

Every tool passes `mo test` in CI (`ci/check_programs.py` and `ci/check_tools.py`), and any of them can be installed onto `$PATH` as pre-verified bytecode with `mo install`.

## Daily-driver & API CLI tools

| Tool | Entry | What it does |
| --- | --- | --- |
| **`mox`** | [`mox/main.mo`](mox/main.mo) | Human-friendly HTTP/HTTPS client (`httpie`/`xh` in Mo) with shorthand request items (`Header:Val`, `q==val`, `n:=42`), formatted output, local SQLite request history, and response diffing. |
| **`moloc`** | [`moloc/main.mo`](moloc/main.mo) | Fast polyglot source, comment, and blank line counter (`tokei`/`scc` in Mo) built on `ReadFiles.walk`, block-comment tracking, and `Unicode16` widths. |
| **`moq`** | [`moq/main.mo`](moq/main.mo) | Pipe JSON, NDJSON, or CSV to slice by path (`jq`-style, with `[]` fan-out) or auto-load into SQLite (`sqlite-utils`-style) for one-liner SQL analytics. |
| **`gh-sync`** | [`gh-sync/main.mo`](gh-sync/main.mo) | GitHub repository issue and pull-request sync into SQLite over HTTPS with cycle-time analytics. |
| **`dev-pulse`** | [`dev-pulse/main.mo`](dev-pulse/main.mo) | Multi-source developer feed aggregator fetching Hacker News, Lobsters, and GitHub feeds over HTTPS with SQLite URL deduplication and search. |
| **`work-etl`** | [`work-etl/main.mo`](work-etl/main.mo) | Cursor-paginated API ETL CLI ingesting JSON invoices into SQLite and emitting exact-`Decimal` revenue reports in Markdown, CSV, or JSON. |
| **`mo-harness`** | [`mo-harness/README.md`](mo-harness/README.md) | Confined autonomous coding-agent harness combining `Files.within`, Landlock/seccomp-confined `Command`, host-allowlisted `Http`, a `TaskCoordinator` `actor`, and replayable SQLite trajectories. |

## Repository & learnability tools

`linkcheck`, `notioncopy`, and `tally` were originally written in Mo by fresh agent sessions during the learnability rounds and are maintained as live repository tools checked by `ci/check_tools.py` (the `bench` group of `ci/check.sh`):

| Tool | Entry | What it does |
| --- | --- | --- |
| **`linkcheck`** | [`linkcheck.mo`](linkcheck.mo) | Verifies every relative Markdown link and heading anchor across all tracked files in the repository. |
| **`notioncopy`** | [`notioncopy.mo`](notioncopy.mo) | Splits Markdown documents (such as `docs/reference/README.md`) into byte-limited pieces with Adler-32 checksums. |
| **`tally`** | [`tally.mo`](tally.mo) | Reports first-retry compile-error repair rates from recorded learnability session logs (`learnability/README.md`). |

## Running and installing

From the repository root, with `target/release/mo` built (`cargo build --release --bin mo`):

```sh
./target/release/mo run tools/moloc/main.mo
./target/release/mo run tools/linkcheck.mo
./target/release/mo test tools/mox/main.mo
./target/release/mo install tools/moloc/main.mo          # installs ~/.local/bin/moloc
python3 ci/check_tools.py target/release/mo              # runs the three repo tools + hidden tests
```

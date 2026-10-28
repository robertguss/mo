# Roadmap

Progress is tracked by phase, not by date. Each phase begins with a design
step for anything a Mo user would notice.

| Phase | Goal | Release |
| --- | --- | --- |
| 0. Reset | A clean foundation | |
| 1. Design the core | Decide what core Mo looks like and means | |
| 2. Agents can write it | A toolchain agents use every day | Internal milestone |
| 3. Services | Long-running services and MCP servers in Mo | |
| 4. Agents can see inside it | A runtime agents can operate | **v0.1**, the first public release |
| 5. It evolves while running | Typed live upgrades | v0.2 |
| 6. Intent and autonomy | People approve intent; agents ship the rest | v0.3 |
| 7. Fast | Fast enough for serious services | v0.4 |
| 8. Ecosystem | Others build and share | 1.0 |

**Current phase: 3, Services.** Phase 0 (Reset) finished on 11 Oct 2026, and
Phase 1 (Design the core) on 12 Oct 2026, with the Language Reference v0.1,
Architecture v0.1 and Mo Core in Lean. Phase 2 (Agents can write it) finished on
24 Oct 2026, with all 44 items of its acceptance checklist met on main
[`1db2d16`](https://github.com/robertguss/mo/commit/1db2d16015d65e1beed0e865e83b7a97ac7a9a0b);
Robert signed it off the same day.

## Phase 2 on main

Checked on 24 Oct 2026 against main at
[`4958074`](https://github.com/robertguss/mo/commit/49580746ad3257e15b44c3d2c595b2c0e831b0ee),
Notion's [Status](https://www.notion.so/3fd700320c45810ca58ddf73fb6fc8c2) and
[Changelog](https://www.notion.so/3fd700320c4581d083f7ca20eb6e0903).
This is a snapshot of what Phase 2 delivered. Its acceptance checklist, every item
met, is in Notion's [Phase 2 page](https://www.notion.so/3f6700320c4581c4b7c4dafa8f459b52).

- The C toolchain checks, lowers, compiles, verifies and runs source programs.
  `mo check`, `run`, `test`, `format`, `fix`, `emit` and `guide` work.
  `mo emit FILE -o OUT` writes binary bytecode; no command runs that file yet.
- All twelve samples pass `mo check` and plain `mo test`. Eight have a `main`
  and run; the other four are libraries, for which `mo run` reports E0079.
- `mo test` generates and shrinks structural inputs, maps and sets, and generates
  fake capabilities and hidden types through module builders. Built hidden values
  are not shrunk. A builder may take other hidden types, up to three levels deep
  and never in a cycle (MO-127), and a built input keeps the hidden value its
  call returned (MO-129). Generic promises run at each concrete instantiation
  already in the program, without inventing type combinations (D273).
- The library's twelve modules in `lib/` are FileError, Files, Json, List, Map,
  Option, ReadFiles, Result, Set, Text, Time and Unicode16; each reserves its
  name (D286). The `drivers` group runs `mo test` on every one (#182). MO-135's
  probe ([section-14-inventory.md](status/section-14-inventory.md)) found every
  reference section 14 function running as a dot call; qualified `T.to_text`
  and `T.compare`, as calls and values, run for every type with the ability
  since MO-144 (#208, D285) and MO-149 (#214, D297).
- `tests/conformance/rules.tsv` maps each of the reference's blocks to
  golden programs, exact errors, other drivers, cost tests or an untestable
  reason (238 rows in the 24 Oct snapshot, with no gaps after MO-140; 253
  rows on this main, with one gap at 11.5/4 for E1408). It is a map with a
  coverage guard; the Phase 2 checklist decides whether it counts as the
  conformance suite.
  `sort`'s n log n cost is measured by counting VM instructions (MO-146, D294).
- 211 golden programs in the 24 Oct snapshot (214 on this main), the
  differential checks, fuzz targets and bounded CBMC proofs exist. The
  recorded hour-long fuzz runs had no crash. One million
  generated Core programs agreed between the VM and Lean on main `267370f`,
  with no disagreement or budget cutoff (#190,
  [mo119-core-differential.md](status/mo119-core-differential.md)).
  `mo format` reaches a fixed point on every tracked Mo program (#191).
- The three tools' final round passed 5 of 6 attempts, with 10 of 10 compile errors
  fixed on the first retry ([#181](https://github.com/robertguss/mo/pull/181)).
  Maintained copies are in [tools/](../tools/README.md), and the `drivers` group
  runs them: linkcheck over the repository's Markdown, tally over every recorded
  round's session logs, and notioncopy on the reference (MO-139, D287).
- Benchmark v0 has 20 frozen tasks, 4 held out. Round 1 (#193) is the baseline:
  all 40 programs pass the hidden tests, and 37 of 37 compile errors were fixed
  within three attempts, but only 29 of 37 (78.4%) on the next attempt. Round 2
  (#210), after MO-137's messages, fixed 26 of 37 (70.3%) on the next attempt.
  Round 3 (#215), after the guide told agents to fix every reported error before
  running again (MO-147, D296), fixed 37 of 38: 97.4%, Wilson interval
  [86.5%, 99.5%]. That meets the 85% next-attempt target (D298). Each round is in
  `learnability/bench-v0-round-N/`.
- The release performance baseline is recorded in
  [bench/performance/](../bench/performance/README.md): command timings for all
  twelve samples and three small workloads (#185). It is not a new speed gate.

No Mo check gates pull requests (D270). Agents run
`bash ci/check.sh gcc` locally before opening a PR and report other checks run
and not run. Robert approves merges, except for the
docs-only merges permitted by the repository's workflow.

## Next phase

Phase 3's design is in [services.md](design/services.md). Service syntax and
checking landed in MO-155 (#231); descriptors in MO-156; the C service loop
in `src/vm/service.c` in MO-157; the SQLite store in `src/host/store.c` in
MO-158; the D306 Mo simulation API in `src/vm/simulation.c` in MO-159 (#252).
Still open: CLI start of services (`mo run` still E1408), MCP, and the
week-long done criterion. D256 to D267 narrow it to a job queue used by Mo's
own agents over MCP, with pure handlers, saved state in runtime-embedded
SQLite, timers, supervision and an access token. Done means a week of real
use and a deliberate crash with no lost accepted job. A multi-core scheduler,
an HTTP client, TLS and SQL access for Mo programs wait for later phases.

## Alongside every phase

Four tracks run alongside every phase:

- formal verification of Mo's core in Lean;
- evaluations of how well agents work with Mo;
- security;
- documentation.

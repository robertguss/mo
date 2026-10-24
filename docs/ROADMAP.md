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

**Current phase: 2, Agents can write it.** Phase 0 (Reset) finished on 11 Oct
2026, and Phase 1 (Design the core) on 12 Oct 2026, with the Language Reference
v0.1, Architecture v0.1 and Mo Core in Lean.

## Phase 2 on main

Checked on 24 Oct 2026 against main at
[`aa13b8c`](https://github.com/robertguss/mo/commit/aa13b8c0a9f84a5983b855f798d747b3081e8539),
Notion's [Status](https://www.notion.so/3fd700320c45810ca58ddf73fb6fc8c2) and
[Changelog](https://www.notion.so/3fd700320c4581d083f7ca20eb6e0903).
This is a snapshot, not a declaration that Phase 2 is done. The live acceptance
checklist is in Notion's [Phase 2 page](https://www.notion.so/3f6700320c4581c4b7c4dafa8f459b52).

- The C toolchain checks, lowers, compiles, verifies and runs source programs.
  `mo check`, `run`, `test`, `format`, `fix`, `emit` and `guide` work.
  `mo emit FILE -o OUT` writes binary bytecode; no command runs that file yet.
- All twelve samples pass `mo check` and plain `mo test`. Eight have a `main`
  and run; the other four are libraries, for which `mo run` reports E0079.
- `mo test` generates and shrinks structural inputs, maps and sets, and generates
  fake capabilities and hidden types through module builders. Built hidden values
  are not shrunk. Builders needing other hidden types remain unsupported (MO-127).
  Generic promises run at each concrete instantiation already in the program,
  without inventing type combinations (D273).
- Higher-order library functions and Json are Mo code in `lib/`. Text, Set and
  Unicode16 have library modules and tests. Reference section 14 is not complete;
  [golden-gaps.md](status/golden-gaps.md) lists known lowering gaps. The `drivers`
  group runs `mo test` on every library file (#182).
- BigInt's `abs`, `min`, `max`, `pow`, `floor_div`, `floor_mod` and `to_int`, plus
  `Random.int` and `Random.seeded`, lower and run (#186). Remaining native-call
  coverage, including `Int.compare`'s E1103, is tracked by MO-116.
- The three tools' final round passed 5 of 6 attempts, with 10 of 10 compile errors
  fixed on the first retry ([#181](https://github.com/robertguss/mo/pull/181)).
  These are historical results under that round's frozen tests; #183 later added
  a linkcheck test. They do not substitute for benchmark v0's baseline. Its
  first round is frozen and in progress in `learnability/bench-v0-round-1/`,
  not yet published on main.
- The release performance baseline is recorded in
  [bench/performance/](../bench/performance/README.md): command timings for all
  twelve samples and three small workloads (#185). It is not a new speed gate.
- The golden corpus, differential checks, fuzz targets and bounded CBMC proofs
  exist. The recorded hour-long fuzz runs had no crash. The million-program
  VM-against-Lean run, full conformance audit and daily use of all three tools
  remain Phase 2 work.

No Mo check runs on GitHub, and there is no nightly job (D270). Agents run
`bash ci/check.sh gcc` locally before opening a PR and report other checks run
and not run. Slow checks run by hand. Robert approves merges, except for the
docs-only merges permitted by the repository's workflow.

## Next phase

Phase 3's design is in [services.md](design/services.md), not implemented in
the toolchain. D256 to D267 narrow it to a job queue used by Mo's own agents
over MCP, with pure handlers, saved state in runtime-embedded SQLite, timers,
supervision and an access token. Done means a week of real use and a deliberate
crash with no lost accepted job. A multi-core scheduler, an HTTP client, TLS
and SQL access for Mo programs wait for later phases. Exact service syntax,
budgets, timers and restart policy remain design questions.

## Alongside every phase

Four tracks run alongside every phase:

- formal verification of Mo's core in Lean;
- evaluations of how well agents work with Mo;
- security;
- documentation.

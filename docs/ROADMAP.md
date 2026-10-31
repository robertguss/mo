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

Phases 0 to 2 are finished, and Phase 3 is the current phase.

## Where plans and progress live

Robert's Notion hub, [Mo v2](https://www.notion.so/3f5700320c4581e0aa31cbdff9f114ff),
is the only place for plans, tasks and status:

- [Status](https://www.notion.so/3fd700320c45810ca58ddf73fb6fc8c2): where main
  is, what each phase has delivered, open PRs and what is waiting for a decision.
- [Mo Tasks](https://www.notion.so/657ad0b75b55427aa8310151d0468728): every
  task, named `MO-NN` as in pull request titles.
- [Roadmap](https://www.notion.so/3f6700320c4581c694b0fba5e8801c8b): each
  phase's goal, plan and acceptance checklist.

This repository keeps the designs (`docs/design/`, `docs/reference/`,
`formal/`) and the evidence (tests, checks and each PR's description), not
task lists or status.

## Alongside every phase

Four tracks run alongside every phase:

- formal verification of Mo's core in Lean;
- evaluations of how well agents work with Mo;
- security;
- documentation.

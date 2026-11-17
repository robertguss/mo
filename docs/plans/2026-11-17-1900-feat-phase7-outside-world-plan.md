---
title: Phase 7, Mo Talks to the Outside World - Plan
type: feat
date: 2026-11-17
topic: phase7-outside-world
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-brainstorm
execution: code
---

# Phase 7, Mo Talks to the Outside World - Plan

## Goal Capsule

- **Objective:** Robert can use Mo for his everyday API work. He starts a small Mo project, calls a real API over HTTPS, looks through what comes back, keeps it in SQLite and runs the program as a command-line tool. An agent can do the same from Mo's own guidance.
- **Means:** A network capability with a fake, an HTTP client library that ships with `mo`, a SQLite capability, a command that starts a project, a mo-lang skill, and operating-system confinement of the compile and check children.
- **Authority:** `docs/ROADMAP.md`, the language reference and `AGENTS.md` govern. Robert approves every change to Mo's language and toolchain surface: reference text, commands and error codes. This plan is the phase overview. Each milestone gets its own brainstorm and plan, which start from the requirements here and do not reopen the decisions below. The framework, MoDB and performance are context, not active scope.
- **Open blockers:** None.

---

## Product Contract

### Summary

Phase 7 changes from "Fast" to "Mo talks to the outside world". A Mo program gains two capabilities, the network and SQLite, each with a fake so tests run with no network. An HTTP client library ships with `mo`, and one command starts a new project. The compile and check children of `mo serve` are confined by the operating system. The phase closes when Robert has done one real API spike in Mo, the automated checks pass, and an agent evaluation has scored agents writing API clients in Mo with a new mo-lang skill.

### Problem Frame

Six phases built a language, a toolchain, services, introspection, live upgrades and a review door. All of it was proved on one service of 1,214 lines, `services/task-queue/`, and nobody uses Mo for daily work, Robert included. The roadmap's next phase was speed, but nothing has met a speed limit in use. The only load measurements of a Mo service were taken for the MoDB design, outside this repository.

Robert's daily work is calling APIs. He starts a small Python project with `uv init`, installs httpx and explores an API he is learning. He cannot do that in Mo. A Mo program's capabilities are arguments, environment, console, files, clock and random (`src/base/hostops.def`). It has no network access. The toolchain has no TLS client, which is why `mo push` goes through a loopback tunnel. SQLite is built into `mo` for the service store (`vendor/sqlite`), and Mo programs cannot reach it. `docs/design/services.md` moved "an HTTP client, TLS, and database access for Mo programs" to "a later phase", and no phase claimed them.

Robert also wants Mo to be safe without Rust's compile times. Mo programs are memory-safe already: every value is immutable, nothing is passed by reference, and memory is reference-counted (reference sections 3 and 6.2). The C underneath has no such guarantee. `docs/design/intent.md` records that the check child runs pushed code on the host without a sandbox, so a memory-safety defect in the compiler or the VM is a way around the review door. Reading bytes from the internet adds to that exposure.

### Key Decisions

- **Performance is not the next phase.** (session-settled: user-directed — chosen over "Fast" as the roadmap had it: nothing has met a speed limit in use, and speed work would tune a design that real programs will change.) Governs R19.
- **The phase is built around Robert's API work.** (session-settled: user-directed — chosen over MoDB's data store as the first program: he explores APIs every week, and it is the smaller program.) Governs R1, R2, R3, R17.
- **The phase stops at scripts and command-line tools.** Running services stay as they are. (session-settled: user-directed — chosen over letting services call out and over a first framework: everything else builds on this piece, and real use will show what the framework needs before it is designed.) Governs R10.
- **Safety is one rule and one milestone.** (session-settled: user-approved — chosen over hardening the C core before any network work, over designing in-place demands now, and over a phase about safety alone: Mo programs are already memory-safe, and confinement needs no rewrite.) Governs R11, R12, R13.
- **Mo gets no borrow checker.** Value semantics already give what a borrow checker gives for safety. (session-settled: user-approved — chosen over an ownership checker: it would add no safety, and the speed guarantee it could give is the parked in-place demand.)
- **SQLite now, Postgres later.** (session-settled: user-approved — chosen over both adapters in this phase: SQLite is already built into `mo`, and Postgres needs connections, TLS and its login handshake first.) Governs R7, R8.
- **Exploring a response is part of the phase.** (session-settled: user-approved — chosen over leaving JSON handling as it is: looking through an unknown response is easy in Python and awkward in a strictly typed language, and it decides whether Robert does a second spike.) Governs R6.
- **One command starts a project, and there is no interactive prompt.** (session-settled: user-approved — chosen over a REPL: a spike in Mo is a file run with `mo run`.) Governs R9.
- **The library for HTTPS encryption is chosen in the HTTPS milestone's own brainstorm.** It is the one place R11's rule cannot hold and the riskiest choice in the phase. (session-settled: user-approved — chosen over fixing it in this overview.) Governs R4, R11.
- **The mo-lang skill points at `mo guide` and carries no copy of it.** (session-settled: user-approved — chosen over a skill with its own language guide: two copies would drift, and `mo guide` is checked on every build.) Governs R14.
- **Phase 6's open work stays out.** (session-settled: user-directed — chosen over folding real use through the review door into this phase: command-line programs never go through the door.)
- **The phase closes on Robert's spike, the automated checks and an agent evaluation.** (session-settled: user-directed — chosen over automated checks alone and over the spike without an agent evaluation.) Governs R16, R17, R18.
- **Either answer from Robert closes the phase.** A "no" to R17's question is recorded with what got in the way and becomes input to the next phase. Governs R18.

### Requirements

**Calling an API**

- R1. A Mo program makes HTTP and HTTPS requests through a network capability it lists on `main`, and a program that does not list it cannot reach the network.
- R2. The HTTP client is a library that ships with `mo`, so a new project uses it with nothing to install.
- R3. The client covers a request-and-response API call: method, headers, query, a body in and out, the status and a timeout, with failures returned as values.
- R4. HTTPS verifies the server's certificate by default, and a failed verification is an error the program receives.
- R5. The network capability has a fake that answers from responses a test supplies, so examples, promises and tests of a program that calls an API run with no network.
- R6. A program can explore a JSON response whose shape it does not know: print it readably and take values out by path, without declaring types first.

**Keeping what comes back**

- R7. A Mo program opens a SQLite database through a capability it lists on `main`, runs statements with parameters and reads rows back as Mo values.
- R8. The SQLite capability has an in-memory form for tests.

**Starting and running**

- R9. One `mo` command starts a small project that checks, tests and runs as created.
- R10. A program that uses the network and SQLite runs as a command-line tool through `mo run`, with arguments and an exit status as programs have today.

**Safety**

- R11. New code that reads bytes from the network is written in Mo wherever Mo can do the job, and every piece of new C that reads them has a fuzz target with seeds.
- R12. The compile child and the check child of `mo serve` run under operating-system confinement, so a memory-safety defect that pushed source reaches cannot read or write outside what the child needs.
- R13. A check shows the confinement works by failing when the confinement is removed.

**Agents**

- R14. A mo-lang skill helps an agent write Mo by pointing it at `mo guide` and `mo guide --reference`.
- R15. `docs/agent-guide.md` covers the two capabilities, the client and the project command, so `mo guide` teaches them.
- R16. An agent evaluation scores agents writing API clients in Mo with the skill, with its gate declared before it runs and every session published, as in Phases 4 and 5.

**Closing**

- R17. Robert does one real API spike in Mo against an API he is learning, from start to finish, and records whether he would reach for Mo again and what got in the way.
- R18. The phase closes on R17's answer, the automated checks and R16's result, and no waiting period gates it.
- R19. `docs/ROADMAP.md` names this phase as Phase 7 with release v0.4, and moves "Fast" to Phase 8 and "Ecosystem" to Phase 9.

### Milestones

Each milestone gets its own brainstorm and plan.

| Milestone | Covers | Comes after |
| --- | --- | --- |
| M1. Design step: what a spike looks like in Mo, the two capabilities and their fakes, the shape of the confinement, and the roadmap change. Robert approves it before any code. | R19, and the design of R1 to R13 | Nothing |
| M2. Confinement of the compile and check children | R12, R13 | M1 |
| M3. HTTP without encryption: the capability, its fake, the client library and exploring a response | R1, R2, R3, R5, R6, R11 | M1 |
| M4. HTTPS | R4, R11 | M3 |
| M5. SQLite | R7, R8 | M1 |
| M6. The project command, the guide and the mo-lang skill | R9, R10, R14, R15 | M3, M5 |
| M7. Robert's spike, the agent evaluation and v0.4 | R16, R17, R18 | M4, M6 |

M2 touches the service side and nothing else in the phase does. It can run alongside M3 to M6.

### Actors

- A1. Robert: approves the design and each change to Mo's surface, does the spike and gives the answer that closes the phase.
- A2. An agent writing Mo: builds each milestone, and in the evaluation writes API clients from the skill and the guide.

### Key Flows

- F1. An API spike
  - **Trigger:** Robert wants to learn an API.
  - **Actors:** A1
  - **Steps:** He starts a project with one command. He writes a program whose `main` lists the console, the environment and the network. The program calls the API with a token read from the environment and prints the response readably. He takes the fields he wants out by path, saves rows to SQLite, and runs the program again with arguments as a command-line tool.
  - **Covered by:** R1, R2, R3, R4, R6, R7, R9, R10
- F2. A test with no network
  - **Trigger:** `mo test` runs on a program that calls an API.
  - **Actors:** A1, A2
  - **Steps:** The test builds a fake network with the responses it expects and an in-memory database, passes both to the function under test, and checks the result.
  - **Covered by:** R5, R8
- F3. The agent evaluation
  - **Trigger:** M7 starts.
  - **Actors:** A2
  - **Steps:** The gate and the tasks are declared. Each agent session gets the mo-lang skill and a task to write an API client in Mo. Sessions are scored and published.
  - **Covered by:** R14, R15, R16

### Acceptance Examples

- AE1. **Covers R1.** Given a program whose `main` lists only the console, when its code tries to make a request, then `mo check` rejects the program, because it holds no network capability to call.
- AE2. **Covers R4.** Given a server whose certificate is expired or names another host, when a program requests it, then the program receives an error and no part of the response.
- AE3. **Covers R5.** Given a test with a fake that holds one response, when the program makes a second, different request, then the test sees an error and no real connection is made.
- AE4. **Covers R12, R13.** Given a build with the confinement removed, when the confinement check runs, then it fails. Given the normal build, when the check child tries to open a file outside what it needs or to connect to the network, then the operating system refuses.
- AE5. **Covers R17, R18.** Given Robert finishes the spike and says he would not reach for Mo again, when the checks and the evaluation are done, then the phase closes and what got in the way is recorded.

### Scope Boundaries

Deferred for later:

- A running service that calls an API or a database while handling a message.
- HTTP routes that are not MCP, and the framework like FastAPI that is the north star.
- Postgres.
- More than one request at a time, streaming bodies, HTTP/2 and WebSockets.
- A standalone executable for a Mo program, and an interactive prompt.
- Packages, which stay with "Ecosystem".
- Performance: a release build target, interpreter speed, machine code and in-place demands.
- Fil-C compatibility and the question of rewriting the C core.
- Confinement of `mo serve` itself. R12 covers its two children.
- MoDB and the abilities its design asks for, such as reading and writing bytes at a position in a file, and hashing.
- Data a service keeps outside its one saved state, and the task queue's limit of 5,000 jobs.
- Real use through the review door, which is Phase 6's open work.

### Dependencies and Assumptions

- Phase 6 closes as `docs/ops/live-service.md` records, and v0.3 is tagged before this phase's first code merges.
- The hosts Mo is built and checked on allow the confinement mechanism that M2 picks. This is not verified. `docs/design/architecture.md` notes a feature probe that treats seccomp's `EPERM` as unavailable, so some hosts restrict such calls.
- The load measurements behind the decision on performance are from v0.2.0 at `ef37424` and were not repeated on v0.3.
- The agent evaluation runs detached from the agent session that starts it, as `docs/solutions/workflow-issues/launch-scored-runs-detached-from-the-agent-session.md` describes.

### Outstanding Questions

Resolve Before Planning: none.

Deferred to the milestone brainstorms:

- M1: whether the network capability works at the level of an HTTP request or of a connection, with HTTP written in Mo. R11 favours the second, and its cost is interpreter time.
- M1: what authority the SQLite capability carries: which files it may open, and how that relates to `Files`.
- M2: which confinement mechanism, and whether the development and CI hosts allow it.
- M4: which library provides TLS, how it is built with `mo`, and how it is kept current.
- M6: what the project command is called and what it creates.
- M7: the evaluation's tasks, clients and gate, and how its sessions call an API without the live internet.
- After the phase: where the framework and MoDB sit on the roadmap. Robert decides.

<!-- ce-section: work-relationships -->
### How This Work Fits Together

This plan covers Phase 7 only. The list below is the current understanding of the work around it, not a committed roadmap.

- The framework like FastAPI. Depends on this phase for the client and SQLite. It also needs services that call out and HTTP routes, which this phase defers.
  - Phase 6's open work, real use through the review door, returns with it, because the framework is where services matter again.
- "Fast", now Phase 8. Depends on the programs this phase produces for a workload and numbers. The parked items go with it: in-place demands, a release build and interpreter speed.
- MoDB. Can proceed independently of this phase. Still to decide: its place in the order, which is Robert's.
- "Ecosystem", now Phase 9. Enables others to share libraries like this phase's client. Until then the client ships with `mo`.
- The future of the C core, Fil-C and a rewrite. Still to decide. This phase shares its safety rule (R11) and adds confinement (R12) without settling it.

### Sources

- `docs/ROADMAP.md`: the phase table, and "Fil-C compatibility (Phase 7)" under Later.
- `docs/design/services.md`: the capabilities moved to "a later phase", and the rule that a service's state, handler inputs and results hold no capability.
- `src/base/hostops.def`: every capability operation a Mo program has today.
- `docs/reference/README.md` sections 3, 6.2, 11.2, 13.3 and 13.4: immutable values, how `main` receives capabilities, what operations cost, and the `mo` commands.
- `docs/design/intent.md`, "What the lock does not protect against", and `docs/design/sandbox-properties.md`: the trusted C and the missing confinement.
- `docs/status/mo167-filc-trial.md`: the Fil-C trial that failed.
- `bench/performance/README.md`: the only speed baseline in this repository, and how an optimized `mo` is built.
- `robertguss/modb`, `issues/11-how-fast-and-large-is-a-mo-service.md` and `design.md` section 8: the load measurements and what MoDB's design asks Mo to gain.
- Three outside reviews on 2026-11-17, from Claude Opus 5.5, GPT-6.1 Sol and Gemini 4 Carbon. Each was asked whether performance is the right next phase, without Robert's lean or the author's. All three said no and said to build real programs first.
- `docs/plans/2026-11-12-1604-feat-phase6-intent-autonomy-plan.md`: the previous phase's overview, for the form of a phase plan.
- `docs/solutions/best-practices/qualify-trace-instruments-against-real-clients-first.md`: for the evaluation in M7.

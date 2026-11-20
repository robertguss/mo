# Post-Phase 7 Direction and Settled Decisions

**Status:** Approved and settled with Robert on 2026-11-20 during the whole-project architectural review, while Phase 7 (`Network`, `Http`, `HTTPS`, `Sqlite`) finishes in parallel.

This document records the strategic realignment, security boundaries, language/toolchain cleanup ("Track 1"), and web/service architecture ("Track 2: FastAPI in Mo + BEAM Actors") that govern Mo immediately after Phase 7 lands, before building the real-world crucible suite ("Track 3").

---

## 1. Philosophy and North Star

1. **Primary User First:** Mo is optimized first for Robert's real daily software—API spikes, ETL pipelines, web/API backends, and rewrites of real open-source CLI tools—earning broader developer adoption through real utility rather than synthetic benchmarks.
2. **Matz's Developer Happiness, Written by Agents and Read by Humans:**
   Even when AI agents write 100% of the code, humans still read Mo code and specs to verify logic. Following Ruby and Elixir, `.mo` files must be calm, concise, and a joy to read as code. Any compiler rule, test gate, or digest lock that forces prose bureaucracy, duplicate helpers, or contorted code idioms is a defect in Mo's design.
3. **Rust's Guarantees Without Rust's Friction:**
   Algebraic types, exhaustiveness, explicit `Option`/`Result` with `try`, explicit capability authority, pure value semantics, sub-second whole-program checks, and Perceus-style opportunistic in-place memory reuse—with **no borrow checker**.
4. **BEAM Runtime Architecture:**
   Share-nothing per-process heaps (`mo_process`), deterministic instruction budgets, per-request/per-message fault containment ("let it crash" without taking down the server), local-socket hot code upgrades, and live introspection—extended to concurrent HTTP `api` servers and composable in-memory `actor`s.
5. **Dogfooding Over Synthetic Evaluations:**
   Existing Lean, CBMC, TLA+, sanitizer, and fuzz checks remain green, but new formal-proof and frozen-cohort evaluation expansion pauses until Mo powers real tools and services used regularly.

---

## 2. Track 1: The "Matz & Perceus" Cleanup Pass

### D1. Code-First `promise` Blocks (Revises Reference §12.2)

- **Optional prose label:** `promise ... end` (or `promise on submit ... end` in a service/actor) requires no English string. An optional string (`promise "label" ... end`) remains permitted when a short name helps.
- **Multiple invariants per block:** A single `promise ... end` block may hold one or more `Bool` invariant lines, avoiding repeated blocks on the same function.
- **Drop mandatory `allows` and `forbids` anchors:** Remove `allows` and `forbids` boilerplate (and the top-level setup constants they forced, such as `one_job` / `leased_job` in `services/task-queue/main.mo`).
- **Automatic non-vacuity check in `mo test`:** `mo test` automatically fails a vacuous promise—such as a constant-true tautology or an `a implies b` condition whose antecedent `a` is never `true` across all `examples` and generated inputs.

```mo
fn withdraw(account: Account, amount: Decimal) : Result(Account, AccountError)
  promise
    result is Ok(after) implies after.balance >= 0
    result is Ok(after) implies after.balance == account.balance - amount
  end

  examples
    withdraw(alice(100.00), 30.00) == Ok(alice(70.00))
    withdraw(alice(100.00), 100.01) == Error(InsufficientFunds(short_by: 0.01))
  end

  return Error(InvalidAmount) if amount <= 0 or amount > max_amount
  return Error(InsufficientFunds(short_by: amount - account.balance)) if amount > account.balance
  var after = account
  after.balance -= amount
  Ok(after)
end
```

### D2. Fast, Un-Gated `mo test` + Opt-In Diagnostic `mo test --mutants` (Revises Reference §12.6 & §13.4)

- **Plain `mo test` runs checks only:** `mo test FILE` runs `examples`, `promise`s (with automatic non-vacuity), `test`s, and `service`/`actor` recovery checks, returning immediately with no mutation pass.
- **Drop `E1440` (mandatory stated intent on every exposed function/tool):** Trivial getters (e.g., `fn owner(account: Account) : Text`) and straightforward helpers never require artificial `examples` blocks.
- **Move mutation testing to `mo test --mutants`:** Retain the mutation engine (`src/tools/mutants.c`) as an explicit diagnostic audit tool (`mo test --mutants FILE`) so agents can inspect surviving mutants for real missing edge-case tests without ever rewriting clean `x or 0` idioms into `case` statements or adding step-limit timeout hacks to kill equivalent fast-path mutants.
- **Remove `--against` mutant carry-over machinery:** With plain `mo test` fast again, the incremental mutant carry-over flags and rules are removed.

### D3. Security Boundaries, `mo diff`, and Control-Plane Isolation (Replaces Phase 6 `mo review`)

- **No unattended LLM live deploys:** Mo never wires an autonomous agent that reads untrusted crash/failure inputs directly to live deploy credentials (preventing the **Crash → Prompt-Injection → RCE** exploit chain, where conditional backdoors pass every property test and intent check).
  - **Runtime self-healing** happens automatically via BEAM process isolation: a failing request/message aborts its `mo_process`, rolls back uncommitted state, quarantines a bounded failure artifact, and keeps serving traffic.
  - **Code repair** happens offline: an agent reproduces and fixes the bug via `mo replay --failure ID --fix` in an unprivileged workspace, and **a human reviews the `.mo` diff + `mo diff` report before deploying**.
- **Replace `mo intent` and `mo review` with `mo diff`:**
  - Remove `mo review`, the 3rd `APPROVAL_TOKEN`, SQLite pending-review holding tables (`mo-review-pending`), and transitive helper-body SHA-256 intent digests.
  - Add **`mo diff [OLD] NEW`** (defaulting `OLD` to Git `HEAD`), working uniformly across CLI tools, ETL scripts, libraries, `api` servers, and `actor`s/`service`s to report four architectural deltas:
    1. **Authority delta:** Added/removed capabilities (`Network`, `Sqlite`, `Files`, `Env`) and capability attenuations.
    2. **Public / Wire delta:** Added/removed/changed `expose`d types & signatures, HTTP `api` routes, and `actor`/`service` calls.
    3. **State delta:** Changes to persistent `state` schemas or migrations.
    4. **Behavioral contract check:** Runs `OLD`'s compiled `promise`s and `examples` (using `OLD`'s own compiled helper closures) directly against `NEW`'s functions/handlers—catching semantic regressions and helper-gutting without SHA-256 rigidity or duplicate `shown?`/`wanted?` functions.
- **Move `mo push` and live introspection off public HTTP sockets:**
  - Public HTTP listeners serve **only** application routes—never code-upload or compiler endpoints.
  - Hot upgrades (`mo push`) and live operator queries (`mo query`) bind exclusively to an opt-in **local Unix domain socket** (`--admin-socket PATH`, protected by OS filesystem permissions).
- **Host-scoped `Network` attenuation:**
  - Add `network.allow_hosts([...])` (paralleling `files.within(...)`) so handlers can be restricted to explicit upstream hosts, enforced in `src/host/` and surfaced in `mo diff`.

### D4. Perceus In-Place Reuse, Runtime Speed, and Single-Binary Portability

Fix the compiler and runtime bottlenecks uncovered during the codebase audit so Mo meets Reference §§6.2 & 13.3's constant-time guarantees:

1. **Struct field updates & nested collections (`src/lower/expressions.c`, `src/codegen/`):**
   - Fix `K_BIND` / `field_update` so `s.items = s.items.append(x)` and `s.jobs = s.jobs.set(k, v)` extract/clear the old field reference before evaluating the RHS, keeping `object.count == 1` on nested `List`/`Map`/`Set` values instead of cloning in $O(N)$ on every update.
   - Wire `MO_OP_RESET`, `MO_OP_REUSE`, and `MO_OP_SET_FIELD` in `src/codegen/` (the VM and heap machinery in `src/heap/reuse.c`, `src/heap/value.c`, and `src/vm/execute.c` already exists).
2. **String interpolation folding (`src/lower/expressions.c:914`):**
   - Strip empty `""` literal parts in `K_TEXT` and fold `MO_CORE_TEXT_OP_CONCAT` **left-to-right** so loops like `acc = "#{acc}#{piece}"` preserve `acc`'s `object.count == 1` left buffer and hit `mo_text_concat`'s 2× geometric growth instead of copying $O(N^2)$ into a leading constant `""`.
3. **List and map `for` loops (`src/lower/expressions.c:718`, `src/codegen/generate.c:365`, `src/heap/value.c:519`):**
   - Lower `for x in list` to direct indexed iteration (`0..list.size` + `LIST_GET`) instead of `MO_CORE_LIST_OP_UNCONS` per-element tuple boxing and `mo_list_slice` $O(N^2)$ tail copying on shared lists (`object.count > 1`).
   - Iterate `Map` and `Set` entries by slot/index without materializing intermediate `List((K, V))` heap tuples.
4. **Service state round-trip overhead (`src/vm/service.c:212,246`):**
   - Eliminate the redundant full `MOSV` decode on every state-updating message when only encoding/checksumming is required, and avoid artificial `state->count >= 2` sharing across handler calls.
5. **Release builds & codegen prologue cleanup (`Makefile`, `src/codegen/compile.c:183`, `src/codegen/registers.c:130`):**
   - Add `make release` (`-O2` with `MO_ASSERT` kept active per Tiger Style).
   - Stop emitting unconditional function-entry `MO_OP_CONST` stores for registers only used in cold branches.
6. **Single-binary portability (`lib/` embedding & `mo emit`):**
   - Embed `lib/*.mo` into the `mo` binary (matching `runtime/*.mo` and `docs/reference/README.md`, with an optional `--lib` override for stdlib development) so `mo` can be copied to any path or machine as a self-contained binary.
   - Either support `mo run FILE.bc` or gate `mo emit` until `.bc` loading is supported.

---

## 3. Track 2: "FastAPI in Mo" + Composable BEAM Actors

### D5. Explicit Separation of Concurrent `api` Servers and Stateful `actor`s

- **Separate constructs for separate physics:**
  - **`api ... end`** declares a **concurrent, capability-driven HTTP/REST + OpenAPI server**, where each incoming HTTP request runs in its own isolated `mo_process` heap concurrently.
  - **`service ... end` (evolving into composable `actor Name ... end`)** declares a **serialized, deterministic state machine** (with timers and optional WAL persistence).
- **The BEAM Endgame (One Binary, Zero Redis/Celery):**
  - `api` blocks can declare and call in-process `actor`s (`actor limiter: RateLimiter`) via fast, typed cross-process message copying—combining concurrent I/O web workers with hot stateful actors inside one self-contained Mo binary.

### D6. Built-In Compiler-Derived `Json` Ability

Add `Json` as Mo's sixth built-in compiler-derived ability on `struct`s and `enum`s, usable uniformly across CLI API spikes, ETL scripts, and `api` servers:

- **Operations:** `Json.decode[T](input)` (`Text` or `Bytes`), `Json.decode_strict[T](input)`, `value.to_json`, and compile-time JSON Schema generation.
- **Scalars:** `Int`, `Byte`, and finite `Float` encode/decode as **real JSON numbers** (`42`, `3.14`); `Decimal` (`"19.99"`), `BigInt`, and `Time` (RFC 3339) encode/decode as JSON strings.
- **Enums:** Payload-free `enum` cases map to `snake_case` strings (`InProgress` ↔ `"in_progress"`, using Reference §2.1's word-splitting rule); payload `enum`s map to clean tagged objects.
- **`Option(T)` struct fields:** Both omitted keys and `null` decode to `None`.
- **Lenient external reads vs. strict API inputs:**
  - `Json.decode[T]` **ignores unknown extra object fields** by default so a small Mo `struct` cleanly decodes wide third-party API payloads (GitHub, Stripe, OpenAI).
  - Incoming `api` request bodies use `Json.decode_strict[T]`, **rejecting unknown fields** with structured `422` validation errors to catch client typos.
- **Escape hatch:** Any type may customize `to_json` / `from_json` (just as `Text` permits custom `to_text`) without struct-tag annotations.

### D7. Declarative `api ... end` Design

```mo
api
  sqlite db from "app.db"
  auth CurrentUser via authenticate

  post "/login"     login
  get  "/me"        profile
  post "/users"     create_user
  get  "/users/:id" get_user
  post "/webhooks"  stripe_hook

  status Unauthorized  = 401
  status BadInput(_)   = 400
  status NotFound(_)   = 404
  status EmailTaken(_) = 409
end
```

- **Signature-driven binding & OpenAPI:** Path parameters (`:id` → `id: Int`), `GET` query parameters (`limit: Option(Int)`), `POST`/`PUT`/`PATCH` JSON bodies (`body: NewUser`), and response/error schemas are checked statically at `mo check` time and emit `/openapi.json` + interactive docs automatically.
- **Type-driven auth (`auth T via fn`):** Any route handler listing `user: CurrentUser` automatically executes `authenticate` first, short-circuits auth errors, and marks the endpoint as authenticated in `/openapi.json` and `mo diff`; routes without `CurrentUser` are visibly public.
- **Compiler-checked exhaustive `status` mapping:** Domain functions return pure `Result(T, ApiError)` with no HTTP numbers in business logic; `mo check` verifies that every reachable error variant in the `api` block has a `status` mapping.
- **Raw HTTP escape hatches:** Any handler may accept `req: Request` (headers, query, raw `Bytes` body for webhook HMAC verification) and/or return `Response` (custom status, headers, redirects, HTML, SSE).
- **Zero-socket testing:** `ApiTest` exercises routes, auth, status codes, and JSON validation directly in memory against `Sqlite.in_memory()` and `Network.fake(...)` under `mo test`.

---

## 4. Track 3: Real-World Crucible Suite (Post-Implementation Validation)

Once Phase 7 finishes and Tracks 1–2 are built, Mo will be validated on real software rather than synthetic benchmark cohorts:

1. **Crucible 1 — API Spike & ETL CLI:** A real HTTPS-to-SQLite ingestion/analytics CLI (`Json.decode` + pagination + `Sqlite` upserts).
2. **Crucible 2 — Popular Open-Source CLI Rewrite:** Porting a widely used developer tool (such as an `httpie`/`xh`-style HTTP CLI or `tokei`-style SLOC counter) to benchmark Mo's ergonomics, stdlib completeness, and `-O2` single-binary speed.
3. **Crucible 3 — "FastAPI in Mo" Reference App:** A multi-route HTTP service with `Sqlite`, `auth`, webhook verification, OpenAPI docs, and an in-memory `RateLimiter` `actor`, load-tested with `oha`/`wrk`.

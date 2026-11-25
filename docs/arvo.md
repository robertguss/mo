# Arvo: Full-Stack Web Framework in Mo

> **Named after Arvo Pärt** — *tintinnabuli* for web software: two clear voices in harmony (pure, compiler-verified domain logic and thin, capability-confined web edges), with every piece earning its place.

**Arvo** is a pure-Mo web framework built into the standard library under [`lib/arvo/`](../lib/arvo/) (loaded lazily on `use Arvo.*`). Paired with Mo's declarative `api ... end` server, SQLite WAL storage, in-process `actor`s, and a Vite-built frontend via **Inertia v2**, a single compiled `mo` binary serves a complete modern web application—with **zero Node runtime in production** and **zero external Redis/Celery infrastructure**.

See [`examples/arvo-starter/`](../examples/arvo-starter/README.md) for a complete, runnable full-stack starter application (Mo + Arvo + Inertia v2 + React 19 + TypeScript + Tailwind CSS v4 + Vite).

---

## Architecture

```text
Browser (React / TypeScript / Tailwind + Inertia v2)
  │  HTTP/1.1 keep-alive (Vite HMR in dev; static hashed dist/ in prod)
  ▼
mo serve (multi-worker HTTP server, isolated per-request VM heap)
  ├── static "/assets"   Confined static files (MIME, ETag/304, immutable cache)
  ├── Arvo.* modules     Cookies, encrypted SQLite sessions, CSRF, flash, forms, Inertia v2
  ├── Pure Mo domain     Exact Decimal math, Changeset validation, compiler-checked promises
  ├── Sqlite (WAL)       Typed Sqlite.decode[T], transactions, versioned _arvo_migrations
  ├── actor Name         In-process BEAM-style state machines (SSE hubs, limiters, caches)
  └── job "name"         Durable SQLite background queue (arvo_jobs) + periodic schedules
```

Every incoming HTTP request executes in its own isolated VM process and heap. A handler bug or work-limit stop fails only that request, rolls back any open SQLite transaction automatically, and keeps the server up.

---

## Declarative `api` Integration

Arvo builds directly on Mo's compiler-checked `api ... end` block:

```mo
module Main
use Api
use Routes

api
  port 8080
  sqlite db from "app.sqlite"
  static "/assets" from "public/assets"
  auth Routes.Member via Routes.authenticate

  get  "/login"     Routes.show_login
  post "/login"     Routes.post_login
  post "/logout"    Routes.post_logout
  get  "/"          Routes.home

  scope "/items"
    post ""         Routes.create_item
    post "/:id/run" Routes.run_item
  end

  job "nightly_cleanup" Routes.cleanup every 24.hours

  status Routes.Unauthenticated  = 401
  status Routes.Forbidden        = 403
  status Routes.NotFound(_)      = 404
  status Routes.Conflict(_)      = 409
  status Routes.BadInput(_)      = 422
  status Routes.ServerFailed(_)  = 500
end
```

What `mo check` verifies statically before the server starts:

- **Route & parameter binding:** Path parameters (`:id`), query parameters, strict JSON bodies (`Json.decode_strict[T]`), and cross-module `Module.fn` handlers match their types.
- **Type-driven authentication:** Handlers taking `member: Routes.Member` automatically execute `Routes.authenticate` and short-circuit on failure; handlers taking `Option(Routes.Member)` accept both authenticated and guest requests; handlers omitting it are visibly public.
- **Exhaustive `status` mapping:** Domain handlers return pure `Result(T, AppError)` with no HTTP status numbers mixed into domain logic; `mo check` refuses any unmapped error variant.
- **Automatic OpenAPI 3.1 & docs:** `GET /openapi.json` and interactive `GET /docs` are generated from the checked route and type signatures.
- **Confined static mounts:** `static "/assets" from "public/assets"` serves files strictly inside the configured folder with MIME detection, `ETag`/`If-None-Match` (`304 Not Modified`), `HEAD`, `X-Content-Type-Options: nosniff`, and immutable caching for fingerprinted Vite assets.

---

## Included Modules (`lib/arvo/`)

Every Arvo module is written in pure Mo and tested with `mo test`:

| Module | File | Responsibility |
| --- | --- | --- |
| `Arvo.Cookie` | [`lib/arvo/cookie.mo`](../lib/arvo/cookie.mo) | RFC 6265 cookie parsing and injection-safe `Set-Cookie` builders (`HttpOnly`, `Secure`, `SameSite`, `Max-Age`, `Path`). |
| `Arvo.Session` | [`lib/arvo/session.mo`](../lib/arvo/session.mo) | AES-256-GCM encrypted session cookies backed by revocable SQLite `arvo_sessions` rows. Raw tokens never touch disk—SQLite stores only `Crypto.sha256(raw_token).to_hex`. Supports create, load, rotate, and revoke. |
| `Arvo.Csrf` | [`lib/arvo/csrf.mo`](../lib/arvo/csrf.mo) | Session-bound double-submit CSRF protection compatible with `@inertiajs/react` and Axios (`XSRF-TOKEN` cookie + `X-XSRF-TOKEN` header), verified in constant time. |
| `Arvo.Auth` | [`lib/arvo/auth.mo`](../lib/arvo/auth.mo) | Email normalization, constant-time allowlist checks, scrypt password hashing/verification (`Crypto.hash_password` / `Crypto.verify_password`), dual-cookie login/logout responses, and CSRF guards. |
| `Arvo.Flash` | [`lib/arvo/flash.mo`](../lib/arvo/flash.mo) | AEAD-sealed short-lived `arvo_flash` cookie carrying one-hop notices, alerts, and validation error bags across `303 See Other` redirects. |
| `Arvo.Form` | [`lib/arvo/form.mo`](../lib/arvo/form.mo) | Bounded `application/x-www-form-urlencoded` and `multipart/form-data` request body parsers for forms and file uploads. |
| `Arvo.Inertia` | [`lib/arvo/inertia.mo`](../lib/arvo/inertia.mo) | Inertia v2 server protocol: `X-Inertia` JSON responses, XSS-escaped initial HTML shells, Vite dev HMR vs. production `manifest.json` SHA-256 asset versioning, `409 Conflict` + `X-Inertia-Location` reloads, and `X-Inertia-Partial-Data` partial reloads. |
| `Arvo.Changeset` | [`lib/arvo/changeset.mo`](../lib/arvo/changeset.mo) | Ecto/Ash-style validation accumulator producing `Map(Text, List(Text))` and Inertia `useForm().errors` (`Record<string, string>`) JSON payloads. |
| `Arvo.Migrations` | [`lib/arvo/migrations.mo`](../lib/arvo/migrations.mo) | Versioned SQLite migration runner tracked in `_arvo_migrations` with SHA-256 checksum verification, transactional execution, and trigger-aware SQL splitting. |
| `Arvo.Audit` | [`lib/arvo/audit.mo`](../lib/arvo/audit.mo) | Append-only `arvo_events` audit log with before/after/undo JSON snapshots, lexicographically time-ordered ULID primary keys, and optimistic concurrency helpers (`version` checks). |
| `Arvo.Jobs` | [`lib/arvo/jobs.mo`](../lib/arvo/jobs.mo) | Durable SQLite-backed background job queue (`arvo_jobs`) with transactional `enqueue` / `enqueue_at`, atomic `BEGIN IMMEDIATE` claiming, exponential backoff retries, and stale-lock recovery. |
| `Arvo.WebTest` | [`lib/arvo/web-test.mo`](../lib/arvo/web-test.mo) | Stateful cookie-jar, CSRF, and Inertia v2 test client for zero-socket end-to-end testing with `ApiTest.send`. |

---

## Security Properties

- **Pure-Rust cryptography:** All hashing (`SHA-256`, `HMAC-SHA256`), AEAD encryption (`AES-256-GCM`), constant-time equality, password hashing (`scrypt`), and OS entropy (`SecureRandom`) run in pure safe Rust (`crates/mo-prim` / `crates/mo-host`) with zero OpenSSL linkage.
- **No raw session tokens on disk:** Revocable SQLite session tables store only SHA-256 token digests; cookies are AEAD-sealed and bound to explicit key lengths.
- **Explicit capability boundaries:** Routes only receive the SQLite database, environment, clock, crypto RNG, or narrowed `Files`/`Network` capabilities explicitly wired into them.
- **Sanitized server logs:** `mo serve` logs one sanitized request summary per request—never headers, cookies, or bodies.

---

## Zero-Socket End-to-End Testing

Because every capability in Mo has an in-memory fake, full browser flows—login, CSRF validation, flash redirects, Inertia partial reloads, multipart uploads, SQLite migrations, background jobs, and static `ETag`/`304` responses—run deterministically in milliseconds with zero network sockets:

```sh
mo test examples/arvo-starter/main.mo
```

Background jobs declared with `job "name" handler` are tested deterministically with `ApiTest.run_job(Main, "name", payload, ...)`.

---

## Development & Single-Binary Production

In development, run `vite` for frontend HMR alongside `mo serve --watch` (which recompiles and reloads `.mo` modules automatically on save):

```sh
# Terminal 1: frontend HMR
cd examples/arvo-starter/frontend && npm install && npm run dev

# Terminal 2: Mo backend with live reload
VITE_DEV_URL="http://127.0.0.1:5173" \
ARVO_SECRET_KEY="$(openssl rand -hex 32)" \
ARVO_INSECURE_COOKIES=1 \
  mo serve examples/arvo-starter/main.mo --port 8080 --watch --data /tmp/arvo-data
```

In production, build the Vite bundle once (`npm run build`) and serve static fingerprinted assets, Inertia pages, JSON APIs, background jobs, and SQLite WAL from a single `mo serve` process:

```sh
mo serve examples/arvo-starter/main.mo \
  --bind 127.0.0.1 \
  --port 8080 \
  --workers 8 \
  --data /var/lib/my-app
```

See [`examples/arvo-starter/README.md`](../examples/arvo-starter/README.md) for the complete walkthrough.

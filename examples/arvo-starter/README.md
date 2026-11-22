# Arvo + Inertia v2 Starter

Minimal full-stack reference application combining Mo's declarative `api` server and pure-Mo `Arvo.*` framework (`lib/arvo/*.mo`) with Inertia v2, React 19, TypeScript, Tailwind CSS v4, and Vite—deploying as a single Mo binary + static asset directory with zero Node runtime in production.

## Architecture Demonstrated

- **Cross-module `api` routing (`main.mo` + `routes.mo`):** `scope "/bills"`, `scope "/imports"`, `static "/assets" from "public/assets"`, and `auth Routes.Member via Routes.authenticate` with both optional (`Option(Member)`) and required (`Member`) route bindings.
- **App-level auth & CSRF (`Arvo.Auth`, `Arvo.Session`, `Arvo.Csrf`, `Arvo.Cookie`):** 2-email household allowlist, scrypt password hashing, AES-256-GCM sealed cookies backed by revocable SQLite `arvo_sessions` rows (storing only SHA-256 token digests), and `@inertiajs/react`-compatible `XSRF-TOKEN` / `X-XSRF-TOKEN` double-submit verification.
- **Inertia v2 protocol (`Arvo.Inertia`, `Arvo.Flash`, `Arvo.Changeset`):** Vite dev HMR shell vs production `manifest.json` SHA-256 asset versioning, `409 Conflict` + `X-Inertia-Location` on version mismatch, `X-Inertia-Partial-Data` partial reloads, XSS-escaped `data-page` JSON, and sealed one-hop flash validation errors across `303 See Other` redirects.
- **Multipart uploads, migrations & audit trail (`Arvo.Form`, `Arvo.Migrations`, `Arvo.Audit`):** Bounded `multipart/form-data` CSV import, checksum-verified `_arvo_migrations`, ULID-ordered `arvo_events` audit snapshots, and optimistic concurrency (`version` column + `Audit.ensure_updated`).
- **Zero-socket end-to-end testing (`Arvo.WebTest` + `ApiTest.send`):** Full browser cookie jar, CSRF, Inertia HTML/JSON, static `ETag`/`304`, and session revocation test suite running in memory in milliseconds:

```sh
mo test examples/arvo-starter/main.mo
```

## Development (`mo serve --watch` + `vite`)

Run the Vite HMR server in one terminal:

```sh
cd examples/arvo-starter/frontend
npm install
npm run dev
```

Run `mo serve --watch` in a second terminal with `VITE_DEV_URL` pointing at Vite:

```sh
mkdir -p /tmp/arvo-starter-data/public/assets
ARVO_SECRET_KEY="$(openssl rand -hex 32)" \
ARVO_INSECURE_COOKIES=1 \
VITE_DEV_URL="http://127.0.0.1:5173" \
  mo serve examples/arvo-starter/main.mo --port 8080 --watch --data /tmp/arvo-starter-data
```

Saving any `.mo` file recompiles and reloads the Mo server in milliseconds; saving any `.tsx` file hot-reloads via Vite React Refresh.

## Production (`vite build` + single-binary `mo serve`)

Build the fingerprinted frontend bundle once during CI/deploy, then serve everything (static hashed JS/CSS with `ETag`/`304` + Inertia pages + SQLite WAL) directly from `mo`:

```sh
cd examples/arvo-starter/frontend
npm ci && npm run build

mkdir -p /var/lib/arvo-starter
cp -R ../public /var/lib/arvo-starter/public

ARVO_SECRET_KEY="$(cat /etc/arvo/secret.hex)" \
ARVO_ALLOWLIST="ada@household.local,grace@household.local" \
  mo serve examples/arvo-starter/main.mo \
    --bind 127.0.0.1 \
    --port 8080 \
    --workers 8 \
    --data /var/lib/arvo-starter
```

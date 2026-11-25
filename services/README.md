# Mo Services

Six real-world services and servers written in Mo, demonstrating Mo's concurrent `api` servers, composable in-process BEAM-style `actor`s, SQLite storage, HTTPS capabilities, and durable MCP `service` runtime.

## Services

| Service | Entry | Kind | What it demonstrates |
| --- | --- | --- | --- |
| [`mo-gateway`](mo-gateway/main.mo) | `services/mo-gateway/main.mo` | `api` + `actor` | Local/team LLM proxy, prompt cache, SSE streaming, and exact sub-cent 128-bit `Decimal` token budgeting via an in-process `Budget` actor and SQLite telemetry. |
| [`mo-pocket`](mo-pocket/main.mo) | `services/mo-pocket/main.mo` | `api` + `actor` | Typed single-binary backend-in-a-box: SQLite user authentication, collection CRUD with ownership rules, automatic OpenAPI 3.1, and a `RealtimeHub` SSE actor. |
| [`mo-pulse`](mo-pulse/main.mo) | `services/mo-pulse/main.mo` | `api` + 3 `actor`s | Self-hosted error tracker, batched structured log sink with SQLite inverted full-text search, and HTTPS uptime monitor composing `AlertDedup`, `BatchWriter`, and `UptimePinger` actors. |
| [`mo-pipe`](mo-pipe/main.mo) | `services/mo-pipe/main.mo` | `api` + `actor` | Single-binary scheduled API sync and ETL daemon with a `PipeScheduler` actor, SQLite watermark cursor tracking, schema-drift alerts, and a status API. |
| [`mo-vcr`](mo-vcr/main.mo) | `services/mo-vcr/main.mo` | `api` + `actor` | Recording HTTPS proxy, deterministic cassette replay server, chaos-injection actor, and SQLite payload drift inspector. |
| [`task-queue`](task-queue/main.mo) | `services/task-queue/main.mo` | durable MCP `service` | Durable MCP task queue with lease expiry, retry limits, SQLite snapshot + recovery-log persistence, crash supervision, live operator views, and typed live upgrades. |

## Testing and running

Every service passes `mo test` with zero external sockets or disk side effects (using `ApiTest.send`, `Actor.start`, `Simulation.start`, `Sqlite.in_memory()`, and `Network.fake(...)`):

```sh
mo test services/mo-gateway/main.mo
mo test services/mo-pocket/main.mo
mo test services/mo-pulse/main.mo
mo test services/mo-pipe/main.mo
mo test services/mo-vcr/main.mo
mo test services/task-queue/queue-tests.mo
```

Start an `api` server locally with `mo serve`:

```sh
mkdir -p /tmp/mo-pocket-data
mo serve services/mo-pocket/main.mo --port 8080 --data /tmp/mo-pocket-data
```

Start the durable MCP `task-queue` service with a token and optional Unix admin socket:

```sh
mkdir -p /tmp/task-queue-data
TASK_QUEUE_TOKEN="change-me" \
  mo serve services/task-queue/main.mo \
    --port 8000 \
    --data /tmp/task-queue-data \
    --admin-socket /tmp/task-queue-admin.sock
```

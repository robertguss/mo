# Examples

Twelve core sample programs covering the Language Reference, plus the full-stack Arvo + Inertia v2 starter application. Every `.mo` file parses with the executable grammar (`make -C grammar grammar`) and passes `mo check` and `mo test` in CI.

## Core samples

| Program | What it does | What it exercises |
| --- | --- | --- |
| [`greet.mo`](greet.mo) | Greets the name given on the command line, or the world | `main` and its capabilities, `Option`, interpolation |
| [`wc.mo`](wc.mo) | Counts lines, words, and bytes in files, like Unix `wc` | reading files, failures, numbers, exit status |
| [`find.mo`](find.mo) | Prints the lines that contain a phrase, like `grep` | parsing arguments, searching text, `case` on text |
| [`expenses.mo`](expenses.mo) | Totals a CSV of expenses by category, largest first | text to numbers, exact `Decimal` money, maps, sorting |
| [`json-query.mo`](json-query.mo) | Reads JSON, formats it, and answers paths like `items.0.name` | `use Json`, `Option`/`Result`, fake `Console` |
| [`todo.mo`](todo.mo) | A to-do list kept in a file | records, commands as an `enum`, writing files |
| [`account.mo`](account.mo) | A bank account's deposits, withdrawals, and transfers | `promise`s, `examples`, opaque types |
| [`rate-limit.mo`](rate-limit.mo) | A token-bucket rate limiter per client | `Time` and `Duration` values, pure state, `promise`s |
| [`job-queue.mo`](job-queue.mo) | A job queue core: submit, lease, acknowledge, fail, expire | state machine over values, multi-step invariants |
| [`kv.mo`](kv.mo) | A Redis-like store: `SET`, `GET`, `DEL`, `INCR`, and `EXPIRE` | parsing, maps, checked integer limits |
| [`notes-tool.mo`](notes-tool.mo) | An MCP tool handler that searches Markdown notes | narrowed read-only `Files` capability, typed errors |
| [`inventory.mo`](inventory.mo) | Generic list helpers and an aligned text table | generics, abilities, functions as values |

## Full-stack web starter

- **[`arvo-starter/`](arvo-starter/README.md)** — Complete full-stack web application built with Mo's declarative `api` server, the pure-Mo [Arvo](../docs/arvo.md) web framework (`lib/arvo/`), SQLite WAL, and Inertia v2 + React 19 + TypeScript + Tailwind CSS v4 + Vite, with zero-socket end-to-end tests (`mo test examples/arvo-starter/main.mo`).

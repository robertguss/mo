# Sample programs

Twelve programs in Mo, written with the Language Reference v0.1. Each one
parses with the executable grammar (`make grammar`). There is no compiler yet,
so none of them runs; their examples and tests say what they compute.

| Program | What it does | What it exercises |
| --- | --- | --- |
| `greet.mo` | Greets the name given on the command line, or the world | `main` and its capabilities, options, interpolation |
| `wc.mo` | Counts lines, words and bytes in files, like Unix wc | reading files, failures, numbers, exit status |
| `find.mo` | Prints the lines that contain a phrase, like grep | parsing arguments, searching text, `case` on text |
| `expenses.mo` | Totals a CSV of expenses by category, largest first | text to numbers, exact money, maps, sorting |
| `json-query.mo` | Parses JSON, lays it out, and answers paths like `items.0.name` | recursive choice types, recursion, patterns |
| `todo.mo` | A to-do list kept in a file | records, commands as a choice type, writing files |
| `account.mo` | A bank account's deposits, withdrawals and transfers | promises, examples, hidden insides |
| `rate-limit.mo` | A token-bucket rate limiter per client | time as a value, state as a value, promises |
| `job-queue.mo` | A job queue's core: submit, lease, acknowledge, fail, expire | a state machine over values, promises about many steps |
| `kv.mo` | A Redis-like store: SET, GET, DEL, INCR and EXPIRE | parsing, maps, numbers at their limits |
| `notes-tool.mo` | An MCP tool's handler that searches Markdown notes | a narrowed, read-only capability, typed results and errors |
| `inventory.mo` | Generic list helpers and an aligned text table | generics, abilities, functions as values |

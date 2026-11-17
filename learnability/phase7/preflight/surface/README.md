# The proposed surface, type-checked

The two programs in [the design](../../../../docs/design/outside-world.md) were first written
here, against stand-in modules, so that the forms Robert reads are Mo the checker accepts today.

```sh
make CC=gcc
cd learnability/phase7/preflight/surface
../../../../build/gcc/debug/mo check look.mo     # exit 0
../../../../build/gcc/debug/mo check issues.mo   # exit 0
```

| File | What it is |
| --- | --- |
| `look.mo` | The first run of a spike: one request, the response printed readably. |
| `issues.mo` | The finished tool and its two tests with no network. |
| `network.mo`, `sqlite.mo` | Stand-ins for the proposed capability types: ordinary structs with the proposed functions, and no bodies. |
| `http.mo` | A stand-in for the client library: its types and signatures, and no bodies. |
| `json-path.mo` | A stand-in for the lookup functions proposed for `lib/json.mo`. |

## What this shows

Every proposed form parses and type-checks: the request built as a chain, `try` over three
different error types gathered into one enum, values taken out of an unknown `Json` by path, and
parameters passed to SQLite as a list of values.

## What it does not show

- **Nothing runs.** Every stand-in function stops. `mo test issues.mo` reports two stopped tests.
- **The entry function is not `main`.** A stand-in is a struct, and `main` takes only real
  capability types. The entry is called `spike` in `look.mo` and `tool` in `issues.mo`. The
  design shows both as `main`.
- **The real types need no `use` line.** A capability type is always in scope. Here `Network` and
  `Sqlite` are imported, and `sqlite` and the lookup functions with them.

## What the checker taught the design

- The cases of `FileError` are always in scope, so no enum in a program may reuse `NotFound`,
  `NotAllowed`, `NotText` or `Failed` (E0043). The first draft of the error types used `Failed`
  and `NotAllowed`. The proposed error types are therefore not always in scope, or every program
  that has a case called `Refused` or `Closed` would stop compiling.
- `Bytes` has no text form. An enum that holds `Bytes`, as a SQLite value does, cannot be put in
  `"#{...}"` (E0080) unless the library gives it a `to_text`.
- A result that is not `Unit` must be used (E0056). `run` therefore returns `Result(Unit, ...)`,
  and a statement is `try database.run(...)`.
- Two functions in one module may share a name when their first parameters have different
  types. `header`, `json` and `text` each exist for a request and for a response.

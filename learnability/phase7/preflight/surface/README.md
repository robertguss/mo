# The proposed surface, type-checked

The two programs in [the design](../../../../docs/design/outside-world.md) were first written
here, against stand-in modules, so that the forms Robert read were Mo the checker accepted.

This folder is a record. When it was written, `mo check look.mo` and `mo check issues.mo` both
exited 0. They no longer check: `Network`, `Connection` and `NetworkError` are built-in types
now, so a `use Network{...}` line clashes with a name that is always in scope (E0045). The
stand-in `network.mo` is removed for that reason. The built capability is in reference section
14.

| File | What it is |
| --- | --- |
| `look.mo` | The first run of a spike: one request, the response printed readably. |
| `issues.mo` | The finished tool and its two tests with no network. |
| `sqlite.mo` | A stand-in for the proposed SQLite capability types: ordinary structs with the proposed functions, and no bodies. |
| `http.mo` | A stand-in for the client library: its types and signatures, and no bodies. |
| `json-path.mo` | A stand-in for the lookup functions proposed for `lib/json.mo`. |

## What this showed

Every proposed form parsed and type-checked: the request built as a chain, `try` over three
different error types gathered into one enum, values taken out of an unknown `Json` by path, and
parameters passed to SQLite as a list of values.

## What it did not show

- **Nothing ran.** Every stand-in function stops. `mo test issues.mo` reported two stopped tests.
- **The entry function is not `main`.** A stand-in is a struct, and `main` takes only real
  capability types. The entry is called `spike` in `look.mo` and `tool` in `issues.mo`. The
  design shows both as `main`.
- **The real types need no `use` line.** A capability type is always in scope. Here `Network` and
  `Sqlite` were imported, and the lookup functions with them.

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

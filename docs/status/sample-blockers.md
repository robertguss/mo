# Sample blockers after I1 reconciliation

Regenerated from actual `mo check --json examples/<sample>` output for main
`1c28e8b`, private checkpoint `cf0f73b`, and the I1 merged tree. Every totals
record reports `dropped: 0`. Examples are unchanged by I1.

The merged CLI uses the whole-program checker and lowers checked programs for
`mo run` and `mo test`. Imports, constants, captures, failure propagation, specs,
promises, and capability entry parameters are integrated. Remaining errors below
are current staged forms or entry requirements; this merge does not implement
the follow-on sample/library tasks.

## Parent and merged counts

| Sample | main | Private | Merged | Better-parent ceiling |
| --- | ---: | ---: | ---: | ---: |
| account.mo | 3 | 1 | 1 | 1 |
| expenses.mo | 3 | 2 | 2 | 2 |
| find.mo | 2 | 1 | 1 | 1 |
| greet.mo | 1 | 0 | 0 | 0 |
| inventory.mo | 6 | 2 | 2 | 2 |
| job-queue.mo | 9 | 1 | 1 | 1 |
| json-query.mo | 10 | 1 | 1 | 1 |
| kv.mo | 6 | 1 | 1 | 1 |
| notes-tool.mo | 1 | 1 | 1 | 1 |
| rate-limit.mo | 3 | 1 | 1 | 1 |
| todo.mo | 6 | 1 | 1 | 1 |
| wc.mo | 2 | 2 | 2 | 2 |
| **Total** | **52** | **14** | **14** | **14** |

## Current diagnostics

| Sample | Location | Code | Diagnostic |
| --- | --- | --- | --- |
| account.mo | 1:1 | E0079 | the entry module has no main function |
| expenses.mo | 38:24 | E0050 | no visible function has this dot-call name |
| expenses.mo | 55:14 | E1103 | this body rule is not implemented by the staged checker |
| find.mo | 104:32 | E1103 | FileError's library text function is unavailable |
| inventory.mo | 30:9 | E0050 | no visible function has this dot-call name |
| inventory.mo | 40:16 | E1103 | this body rule is not implemented by the staged checker |
| job-queue.mo | 36:21 | E1103 | this body rule is not implemented by the staged checker |
| json-query.mo | 61:60 | E1103 | this body rule is not implemented by the staged checker |
| kv.mo | 53:30 | E1103 | this body rule is not implemented by the staged checker |
| notes-tool.mo | 1:1 | E0079 | the entry module has no main function |
| rate-limit.mo | 35:69 | E1103 | this body rule is not implemented by the staged checker |
| todo.mo | 35:26 | E1103 | FileError's library text function is unavailable |
| wc.mo | 20:29 | E0055 | a call's arguments don't match its parameters |
| wc.mo | 52:30 | E1103 | FileError's library text function is unavailable |

Account and notes-tool lack a main entry, E0079. Find, todo and wc reach the
unavailable installed FileError text library, E1103. Other E1103 rows are the
first unsupported body forms actually reached; later forms may remain hidden.
These counts are complete diagnostics from the current checker, not a claim
that the samples are otherwise runnable.

## Greeting

`examples/greet.mo` is unchanged from both parents. On the merged Mac build:

```text
mo check examples/greet.mo       exit 0
mo run examples/greet.mo Ada     exit 0, Hello, Ada!
mo test examples/greet.mo        exit 0, 3 passed, 0 failed, 0 stopped
```

## Regeneration

Build each parent and the merged tree in independent workspaces, then run:

```sh
build/clang/debug/mo check --json examples/<sample>.mo
```

Count the `errors` field of the `totals` record; assert that `dropped` is zero.
The I1 verification record includes commands, compiler qualifications, and
raw-evidence locations: [i1-verification.md](i1-verification.md).

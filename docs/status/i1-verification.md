# I1 merge verification and pending decision

This draft records the reproducible baseline and language-rule question before
reconciling the private checkpoint. It does not yet contain the requested merge
commit and must not be merged. The same PR will carry the merge once the rule is
settled; do not squash or rebase the accepted checkpoint history.

## Revisions and workspace

Full, non-shallow clone of `https://github.com/robertguss/mo.git`.

- main: `1c28e8bf9ef98a7c06a87be6f8617958077b5793`
- private: `cf0f73bc2477dbc11e13f2a4630e90f6006ddfeb`
- merge base: `3523c4af72188a86a2314c3db4fc14b3668c096d`
- unique commits: 22 on main, 136 on private. All supplied revisions match.
- Isolated clone plus detached parent worktrees; existing checkouts untouched.
- `git merge --no-ff --no-commit origin/phase2/main-capability-entry-private`
  reproduced the supplied 15 conflicted files. The diagnostic merge was aborted
  before any conflict resolution, to publish this decision request first.

## Pending language decision: customizer names

Main's `customizer()` in `src/check/signatures.c` treats every function named
`compare` or `to_text` as a customizer and rejects unrelated helper signatures
with E0082. The private parent's `customizer_candidate()` recognizes a customizer
only from a locally declared nominal first parameter (plus trusted FileError),
and accepts unrelated helpers. The architecture and E0082 golden also disagree.
This is user-visible behavior, not just a textual conflict.

**Robert: should the merged checker retain main's reservation and E0082 for
unrelated `compare`/`to_text` helpers, or retain the private checkpoint's acceptance
of ordinary helpers?** Main is stated to be authoritative for abilities/generics;
the private checkpoint also contains accepted behavior. Per the task's explicit
stop-and-ask rule, no resolution of this language rule has been selected.
No Language Reference edits are proposed.

Reproducer (saved as `main.mo`):

```mo
module Main
fn to_text(n: Int): Text
  "#{n}"
end
fn main(): Int
  to_text(7).byte_count
end
```

`build/clang/debug/mo check --json <probe>/main.mo`: main exits 1 with E0082;
private exits 0 with zero errors. Both report `dropped: 0`.

## Parent verification using verify-mo

Unmodified `make test CC=clang` exits 2 on both parents: existing
`-Wenum-compare-conditional` in `src/bytecode/assemble.c` is an error on Apple
Clang. Homebrew LLVM 22 reproduces it. GCC 16 also fails unmodified builds because
`mkdtemp` is not declared with the selected Darwin feature macros.

To obtain executable baselines without editing either parent's source:

```sh
make -j6 test CC='clang -Wno-error=enum-compare-conditional -D_DARWIN_C_SOURCE'
```

Both parents build, then exit 2 at the same pre-existing `test_check` assertion:
`strcmp(f.diags.items[0].primary.text, lib_helper) == 0`, in `library_roots`
(main line 513, private line 1723). Therefore neither Doctor baseline is green.
These temporary compiler arguments are evidence qualifications, not repository
changes or a claim that the standard gate passes.

Private parent, unchanged `examples/greet.mo`:

- `mo check examples/greet.mo`: exit 0.
- `mo run examples/greet.mo Ada`: exit 0, `Hello, Ada!`.
- `mo test examples/greet.mo`: exit 0, `3 passed, 0 failed, 0 stopped`.

Main: check exits 1/E1103; run and test exit 1/E1400.

## Sample diagnostic baseline

Each count is the `errors` field of `mo check --json examples/<sample>`;
every totals record reports `dropped: 0`. Private matches the requested ceiling.

| Sample | main | Private parent | Merged |
| --- | ---: | ---: | --- |
| account.mo | 3 | 1 | pending |
| expenses.mo | 3 | 2 | pending |
| find.mo | 2 | 1 | pending |
| greet.mo | 1 | 0 | pending |
| inventory.mo | 6 | 2 | pending |
| job-queue.mo | 9 | 1 | pending |
| json-query.mo | 10 | 1 | pending |
| kv.mo | 6 | 1 | pending |
| notes-tool.mo | 1 | 1 | pending |
| rate-limit.mo | 3 | 1 | pending |
| todo.mo | 6 | 1 | pending |
| wc.mo | 2 | 2 | pending |
| **Total** | **52** | **14** | **pending** |

## Verification still pending

- Merge conflict reconciliation and merge commit (blocked on language decision).
- Blast-radius review per reconciled function (none reconciled yet).
- Merged `bash ci/check.sh`, sanitizers, short fuzzing, VM-vs-Lean comparison.
- Merged greet, sample counts, imports/constants/lambdas/try/Option/interpolation/
  specs/capability-entry/promise/deadline/shrinking acceptance evidence.
- Regenerated `docs/status/sample-blockers.md` and merged architecture.

No tasks C1, F5, LW5, or M1 started. No Language Reference changes. No PR merge.

# MO-167 Fil-C trial

Fil-C 0.687 does not pass Mo's unchanged test suite. Keep the nightly configuration
unchanged and revisit compatibility in Phase 7, as
[MO-167](https://app.notion.com/p/403700320c45813a80a4db82b20e9c98) specifies.
The trial found both a test-build incompatibility and runtime traps. Fixing the
build warning alone would not make Fil-C usable for Mo.

## Pinned environment

The trial ran on 25 October 2026, on Linux x86-64, against unchanged main
[`6efbcd7277215edbfc47c349509b6d5b59688bd2`](https://github.com/robertguss/mo/commit/6efbcd7277215edbfc47c349509b6d5b59688bd2).
Architecture section 11, "Testing strategy", defines the test layers.
The experiment follows MO-119's pinned-commit and replay-command format.

- [Fil-C release 0.687](https://github.com/pizlonator/fil-c/releases/tag/v0.687),
  published 23 October 2026.
- Asset `optfil-0.687-linux-x86_64.tar.xz`, SHA-256
  `2dc295715dc3347c1cc1b41f7a937b13a076e651e71db42f3adf00d5fb075bfe`.
- Compiler output: `clang version 20.1.8 (Fil-C 0.687 git@github.com:pizlonator/fil-c 36f07a2f8f8141b3488d7ca20a680fd629886eeb)`.
- The archive's inner `fil.tar.xz` was extracted into `/opt`, producing
  `/opt/fil/bin/filcc`. The installer's optional SSH and system configuration did
  not run. This was a trial installation, not a project setup change.
- Mo's existing C17 debug flags, including `-O0 -g`, `-pthread`, and all warnings
  as errors, were retained. No source, test expectation, or warning was changed.

## Replay

With the pinned toolchain installed at `/opt/fil`, run these commands from the
pinned Mo checkout. Use a fresh `BUILD` directory for each compiler or flag change:
the Makefile does not track compiler versions or command-line flags as inputs.

```sh
make -j4 test-executables CC=/opt/fil/bin/filcc BUILD=build/filcc-mo167
make -k -j4 test-executables CC=/opt/fil/bin/filcc BUILD=build/filcc-mo167
make -k -j1 test CC=/opt/fil/bin/filcc BUILD=build/filcc-mo167
python3 ci/check_cli.py build/filcc-mo167/debug/mo
python3 ci/check_golden.py build/filcc-mo167/debug/mo
```

The first build stops on an error. The second collects the other build failures.
The `-k` test run continues through independent targets after failures.
The ordinary `mo` executable builds, so the two Python drivers can run despite
the resolver-fault target's build failure. These driver invocations select the
Fil-C executable explicitly; `ci/check.sh drivers` discovers only GCC and Clang
build directories.

## Completed results

| Check | Result |
| --- | --- |
| Fil-C build | `mo` and 45 of 46 test executables built. `test_host_resolver` did not build. Both build commands exited 2. |
| Fil-C `make -k -j1 test` | 29 test executables passed, 16 failed, and one could not run because it did not build. Exit 2. All six allocation-failure shards passed. |
| Fil-C `mo --version` | Printed `mo 0.0.0`, exit 0. |
| Fil-C CLI driver | Failed at the guide's greeting test. Driver exit 1; child terminated with signal 5. |
| Fil-C golden driver | 3 of 211 fixtures matched, 208 failed. Driver exit 1. |
| Native GCC baseline, `CI=1 bash ci/check.sh gcc` | All 46 test executables passed. Exit 0, no missing-tool skips. |

The 16 failing unit executables fall into these observed categories:

- Null-capability traps: `test_codegen`, `test_fakes`, `test_heap`, `test_host`,
  `test_host_faults`, `test_inputs`, `test_lower`, `test_prim`, `test_unicode16`,
  `test_vm`, and `test_vm_faults`.
- Stack-overflow traps: `test_check_bodies`, `test_core_check`, `test_core_text`,
  and `test_program`.
- Assertion-death expectations: `test_diag`.

These categories describe the first reported failure in each executable, not
an exhaustive inventory of its defects. A trap prevents later cases in that
executable from running. The trial isolated the cell round trip and `abort()` behavior;
it did not establish the cause of every trap.

## Build failure in the resolver-fault test

All production sources compile. The resolver-fault target force-includes
`tests/host_small_limits.h`, which defines `_GNU_SOURCE` and
`_POSIX_C_SOURCE=200809L`, then includes system headers through `base/limits.h`.
Fil-C's `features.h` changes `_POSIX_C_SOURCE` to `202405L` under `_GNU_SOURCE`.
The four substituted host translation units define it as `200809L` again.
Clang rejects the different redefinition under Mo's `-Werror`:

```text
src/host/path.c:2:9: error: '_POSIX_C_SOURCE' macro redefined [-Werror,-Wmacro-redefined]
```

The same failure affects `src/host/console.c`, `real-files.c`, and `host.c` in
that target. It prevents `test_host_resolver.test` from linking. It is a header
and test-build incompatibility, not a reported memory-safety violation.

The following preprocessing control prints `202405L` with Fil-C and `200809L`
with this orb's GCC and system headers:

```sh
printf '' | /opt/fil/bin/filcc -std=c17 -Isrc \
    -include tests/host_small_limits.h -dM -E -x c - | grep '^#define _POSIX_C_SOURCE '
printf '' | gcc -std=c17 -Isrc \
    -include tests/host_small_limits.h -dM -E -x c - | grep '^#define _POSIX_C_SOURCE '
```

No warning suppression or compatibility patch was applied.

## Runtime-cell pointer round trip

The CLI driver traps while testing the agent guide's greeting program:

```text
filc safety error: cannot read pointer with null object.
semantic origin:
    (mo) src/heap/reuse.c:87:5: mark
```

The stack continues through `mo_value_make_immortal`, VM image initialization,
and `mo_test_program`. Python records signal 5 as return code `-5`.

A smaller reproduction uses Mo's public cell API and no VM, heap allocation,
or parser. Save this as `/tmp/mo167-cell.c`:

```c
#include "heap/heap.h"
#include <stdio.h>

int main(void) {
    struct mo_object object = {.count = 37, .layout = 0};
    struct mo_cell cell = mo_cell_ref(&object);
    struct mo_object *decoded = mo_cell_get_ref(cell);
    if (decoded != &object)
        return 2;
    return printf("%u\n", decoded->count) < 0;
}
```

```sh
/opt/fil/bin/filcc -std=c17 -O0 -g -Isrc /tmp/mo167-cell.c \
    src/heap/cell.c src/base/base.c -o /tmp/mo167-cell-filc
/tmp/mo167-cell-filc
gcc -std=c17 -O0 -g -Isrc /tmp/mo167-cell.c \
    src/heap/cell.c src/base/base.c -o /tmp/mo167-cell-gcc
/tmp/mo167-cell-gcc
```

GCC prints `37` and exits zero. Fil-C passes the pointer equality check, then
traps on `decoded->count` with `cannot read pointer with null object`. The shell
reports exit 133. The pointer's address survives, but its Fil-C capability does
not. Equality alone cannot validate this round trip.

A control using the same `struct mo_cell` and the same two `memcpy` operations,
but passing the cell by address to store and load functions, prints `37` under
Fil-C. This narrows the incompatibility to the by-value cell path; it does not
establish which compiler transformation drops the capability. No production
cell layout or API was changed. This is a reproduced Fil-C compatibility
failure, not evidence by itself of an invalid lifetime or access in native Mo.

Save the by-address control as `/tmp/mo167-cell-address.c`:

```c
#include "heap/heap.h"
#include <stdio.h>
#include <string.h>

static void store(struct mo_cell *cell, struct mo_object *object) {
    memcpy(cell->bytes, &object, sizeof object);
}

static struct mo_object *load(const struct mo_cell *cell) {
    struct mo_object *object;
    memcpy(&object, cell->bytes, sizeof object);
    return object;
}

int main(void) {
    struct mo_object object = {.count = 37, .layout = 0};
    struct mo_cell cell = {0};
    store(&cell, &object);
    struct mo_object *decoded = load(&cell);
    if (decoded != &object)
        return 2;
    return printf("%u\n", decoded->count) < 0;
}
```

```sh
/opt/fil/bin/filcc -std=c17 -O0 -g -Isrc /tmp/mo167-cell-address.c \
    -o /tmp/mo167-cell-address
/tmp/mo167-cell-address
```

## Instrumentation control

A separate C program allocates one `int`, writes `37` through a volatile index,
prints that element, and frees the allocation. Index zero prints `37`. Index
16 traps with `cannot write pointer with ptr >= upper` and shell exit 133.
This confirms that the installed compiler and runtime enforce bounds in the
tested executable.

The initial negative control used index one and did not trap. Fil-C documents
that [allocations round up to 16 bytes](https://fil-c.org/invisicaps_by_example).
The diagnostic's lower and upper bounds also differ by 16 bytes. A four-byte
write at offset four remains inside that capability. That control is not a
successful detection of a source-level one-past-allocation write.

This replay combines the original two controls. Save it as `/tmp/mo167-bounds.c`:

```c
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    (void)argv;
    int *value = malloc(sizeof *value);
    if (value == NULL)
        return 2;
    volatile unsigned index = argc > 2 ? 16u : argc > 1 ? 1u : 0u;
    value[index] = 37;
    int result = printf("%d\n", value[index]);
    free(value);
    return result < 0;
}
```

```sh
/opt/fil/bin/filcc -std=c17 -O0 -g -Wall -Wextra -Werror \
    /tmp/mo167-bounds.c -o /tmp/mo167-bounds
/tmp/mo167-bounds
/tmp/mo167-bounds one
/tmp/mo167-bounds one sixteen
```

The first two invocations print `37` and exit zero. The third traps.

## Other failure categories

The unit run also reports Fil-C stack overflows in `test_check_bodies`,
`test_core_check`, `test_core_text`, and `test_program`. Their backtraces include
recursive parser or checker paths. These traps are distinct from
null-capability accesses. The trial did not establish whether stack sizing or
Fil-C's instrumentation explains them, and did not increase stack limits.

`test_diag` fails 11 assertion-death checks. Its subprocess helpers require
`SIGABRT`, but this Fil-C runtime handles `abort()` with `SIGTRAP`. This minimal
control prints `filc user error: abort(3) called` and exits with shell status 133:

```sh
printf '#include <stdlib.h>\nint main(void) { abort(); }\n' > /tmp/mo167-abort.c
/opt/fil/bin/filcc /tmp/mo167-abort.c -o /tmp/mo167-abort
/tmp/mo167-abort
```

That is a signal-contract mismatch, not evidence that Mo stopped asserting.
The test's required signal was not relaxed.

The complete golden driver checked 211 fixtures and rejected 208. Its failure
output contains 207 null-capability traps and one stack-overflow trap. Only the
three checker-rejection fixtures `mod-type-bare-clash-import`,
`mod-type-bare-clash-local`, and `mod-type-bare-private` matched. The CLI driver
stopped at the greeting-program trap, so later CLI checks were not exercised.

## Scope and follow-up

This is a compatibility trial, not a performance benchmark or a proof of memory
safety. Fil-C's own instrumentation is distinct from Mo's ASan and UBSan runs.
No Fil-C fuzz, Lean differential, CBMC, or concurrent rename-stress run was
attempted. The Python CLI and golden drivers do not cover every driver group.

A Phase 7 retry needs to resolve the runtime-cell round trip, the resolver-fault
header ordering, and the remaining failing tests before proposing nightly
adoption. Preserve the existing warning and test contracts. A passing compiler
build or pointer equality check would not meet that acceptance condition.

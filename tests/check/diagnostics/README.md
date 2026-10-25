# Loading, module and body diagnostics

Each `.mo` input has an adjacent `.diag` expectation. Loading errors have a
folder with an entry file; the expectation is next to that folder. Interface
errors use `interfaces/money.mo` as their second module. Each fixture must
produce exactly one diagnostic.

The expectations record the public diagnostic fields, not a terminal rendering:
code; file and half-open byte span; reference section; headline; reason; primary
label; secondary labels; and fixes. `-` means absent. An edit's span is followed
by its replacement in quotes; `""` deletes that span. The test reads the files
without regenerating expectations. Stream 4 can use the same inputs and field
expectations; these are not that wire format.

Allocation and large generated limits are exercised in `test_check.c` and
`test_check_bodies.c`, rather than storing megabytes of fixtures. Body errors
cover names, unused values and failures, numeric ranges, and context-specific
type mismatches with expected/found labels, record/case fields, hidden insides
and pattern coverage. Syntax diagnostics remain in the
syntax corpus. The staged body checker does not yet certify all language rules;
`check.h` and architecture 3.3 list the remaining stages explicitly.

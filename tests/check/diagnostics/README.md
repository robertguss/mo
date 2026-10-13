# Module diagnostics

Each `.mo` input has an adjacent `.diag` expectation. Loading errors have a
folder with an entry file; the expectation is next to that folder. Interface
errors use `interfaces/money.mo` as their second module. Each fixture must
produce exactly one diagnostic.

The expectations record the public diagnostic fields, not a terminal rendering:
code; file and half-open byte span; reference section; headline; reason; primary
label; secondary labels; and fixes. `-` means absent. An edit's span is followed
by its replacement in quotes; `""` deletes that span. The test reads the files
without regenerating expectations. Stream 4 can use the same inputs and field
expectations when its JSON renderer exists; these are not that wire format.

Allocation and large generated limits are exercised in `test_check.c`, rather
than storing megabytes of fixtures. Syntax diagnostics remain in the syntax
corpus. No body-typing errors are covered yet.

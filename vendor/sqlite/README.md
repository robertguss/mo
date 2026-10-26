# SQLite 3.54.1

`sqlite3.c` and `sqlite3.h` are unmodified files from
[SQLite's amalgamation archive](https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip).
The archive's SHA3-256 is
`92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`.
The source ID is
`2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76`.

SQLite is in the public domain. Its source headers contain the dedication;
see [SQLite copyright](https://www.sqlite.org/copyright.html). Mo's licences do
not replace that dedication.

The Makefile compiles this separately from Mo's warning and formatting rules,
but includes it in sanitizer and fuzz builds. `SQLITE_ZERO_MALLOC` removes the
default system allocator. The host installs `base/store-memory` callbacks with
`SQLITE_CONFIG_MALLOC` before initializing SQLite. Extension loading is omitted,
double-quoted string literals are disabled, and SQLite remains thread-safe.

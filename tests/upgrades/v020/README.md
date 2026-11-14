# Actual v0.2.0 task-queue folder with upgrade history

`service.sqlite` was produced by a clean `git archive v0.2.0` build, not by
rewriting a current database to look old. The tag resolves to
`3b91e55b2dcb1ca3c3dca06fbdb6d1ab3e699af5`. `provenance.json` records the binary
and database SHA-256 digests.

The executable was built in an isolated temporary extraction with
`make -j8 CC=gcc` on Linux x86_64, using the default debug configuration. It
first served `first/main.mo` (the tag's `services/task-queue/main.mo`) with a
fresh data directory, `--port 0` and a test-only `QUEUE_TOKEN`, and SIGTERM
stopped it. It then served `main.mo`, the same program with `max_page_jobs`
lowered from 100 to 50, with `--upgrade`, which activated it as ordinal 1 and
wrote one `upgrades` row; SIGTERM stopped it again. `job-queue.mo` is the tag's
file, unchanged.

The fixture has SQLite `user_version=2`, an active program at ordinal 1 and one
upgrade row. A build that serves it converts it to version 3 in the same
transaction that adopts the program under its own build: the review setting
starts on `everything`, nothing is approved, the upgrade rows stay, and the
review record gains one conversion row. v0.2.0 then refuses the folder, because
it refuses any `user_version` above 2.

Tests must copy this directory before opening it. The original binary is not
committed; its hash establishes the producer used for this evidence.

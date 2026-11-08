# Actual v0.1.0 task-queue folder

`service.sqlite` was produced by a clean `git archive v0.1.0` build, not by
rewriting a current database to look old. The tag resolves to
`fad1467dcbd0d34a487521c5294729badbb4af40`. `provenance.json` records the binary
and database SHA-256 digests and the successful submit response. `main.mo` and
`job-queue.mo` are the exact source files from that tag.

The executable was built in an isolated temporary extraction with
`make -j4 CC=gcc` on Linux x86_64, using the default debug configuration. It
served the archived `services/task-queue/main.mo` with a fresh data directory,
`--port 0`, and a test-only `QUEUE_TOKEN`. One authenticated MCP `submit` call
passed `{"payload":"legacy-preserved"}`, returning job id `1`; SIGTERM then
stopped the service with status 0.

The fixture has SQLite `user_version=1`, snapshot sequence 0, and two committed
log records: the startup expiry timer and the submit. The initial prediction
of one log record was wrong because startup queues an expiry tick. The checked
fixture asserts two, rather than discarding the extra runtime event. No program
or upgrade metadata has been added to this original fixture.

Tests must copy this directory before opening it. Adoption must replay both
records and preserve the submitted payload. An image mismatch without
`--adopt`, or a replay checksum mismatch, must leave the copy unchanged.
The original binary is not committed; its hash establishes the producer used
for this evidence, not reproducible-build identity across hosts/toolchains.

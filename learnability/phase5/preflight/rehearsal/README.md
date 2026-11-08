# Offline rehearsal verification

Predictions before implementation: the exported snapshot and both original bundles
remain byte-identical on success and refusal. Missing migration refuses; supplying
the rename migration validates the migrated snapshot, never the initializer.
A different build or old image refuses before migration. Historical generated
UpgradeOld declarations must be supplied as a separate bounded MOCC context for
an old source that retains its migration body; the source digest remains MOSB-only.
Rehearsal uses live admission work and heap limits, not checked-restart limits.
Success is explicitly rehearsal, never acceptance by a running service.

## Observations and failed assumptions

The first migration fixture used `UpgradeOld.Main.Box`, which the language
parser refused. Generated entry declarations are imported with `use UpgradeOld`
and named `UpgradeOld.Box`; correcting the fixture reached the migration runner.
The existing `Served` test helper assumed the first stdout line was a listening
announcement. Checked restart prints its report first; the rehearsal fixture
now consumes those report lines before reading the announcement.
A later offline test failed its executable digest assertion because its test
binary was rebuilt while it ran. This is not an admission failure; final builds
and the full drivers are serialized. Tests were added alongside implementation,
not presented as a red-before-implementation result.

The new offline driver cases exercise missing migration, migrated validation,
wrong old image, wrong build with both digests, malformed context, historical
context on a subsequent tool-only candidate, and a second explicit migration.
The fixtures compare export bytes and the complete service-folder file/byte map
before and after rehearsal. They do not establish operating-system access-time
preservation or absence of compiler scratch files. The existing MOSB, MOCC and
MOEX readers retain their existing framing/fuzz boundaries; this adds no wire
format. Activation fault injection and streaming remain later work.

## Independent review and correction

The completed Compound Engineering review (`20261108-115553-b8113aae`)
covered correctness, project standards, testing, maintainability, security,
API contracts, reliability and agent use. It found that successful rehearsal
reports inherited an activation ordinal of zero. Rehearsal does not activate a
program, and the export supplies no ordinal. Reports now omit `ordinal` while
retaining the exported `head`; the offline driver checks this after a prior
activation as well as on the initial export.

The review also noted a test limitation: the initial exported count and the old
program's initializer are both zero. The trap case distinguishes that input from
the candidate initializer (999), but does not independently distinguish it from
the old initializer. Historical-context and second-migration cases run against
an export after a real checked restart. No broader state-substitution proof is
claimed by these finite cases. The implementation passes the decoded export's
state bytes directly to the shared migration runner.

The post-review combined run passed GCC and Clang units and all drivers, then
failed static formatting on the new `snprintf` line. Clang-format 14.0.6 joined
that call's arguments onto the canonical line; no tokens or behavior changed.

## Final checks

- `bash ci/check.sh gcc static`: `check.sh finished: pass`, with no skips,
  after the formatting correction (`build/check-u3-release.txt`). This includes
  GCC units, dependency rules, library mapping, format checks and clang-tidy.
- The preceding `bash ci/check.sh gcc clang drivers static` completed GCC and
  Clang units and every driver, including the new offline rehearsal tests and
  CLI golden, before its static formatting failure (`build/check-u3-final.txt`).
  Those behavior results remain applicable after the whitespace-only correction.
- The earlier `bash ci/check.sh gcc drivers static` and `bash ci/check.sh clang`
  both ended `check.sh finished: pass` before the ordinal review correction.

Build logs are local and untracked. The commands and bounded observations here
are the durable evidence. No live activation, streaming, activation fault matrix,
or later-milestone verification is claimed.

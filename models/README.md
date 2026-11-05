# Export generation checks

Run `bash ci/check.sh models`. Java and the TLA+ tools v1.8.0 jar are required.
Download `https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar`
to `build/models/tla2tools.jar`, or set `TLA2TOOLS_JAR`. The runner verifies SHA-256
`e20447644bfabd29d6bd0fc2c0eb1473fb301c1a0e4dfe1961938ce5a191edbf` before execution.
Missing tools are a local skip and fail under `CI=1`; a skip is not evidence.
Logs and generated counterexample traces remain under `build/models/`.

The finite instance has one kind, three generation IDs, three sequence/state values,
two-byte artifacts and at most two restarts. IDs represent collision-free random
identities; TLC does not prove operating-system entropy quality. A fetch starts with
its first pin and requests successive pages. Replace and restart preserve that fetch's
stale identity. Expiry/release are abstract actions, not a proof of clock sampling.
`NoMixedGeneration` checks both identity and byte provenance for every accepted page.
`NoReleasedRead` checks a history bit so a later release cannot hide a forbidden read.

The progress instance disables replacement, expiry and restart. It assumes weak fairness
of creation, reading and acceptance; `Completes` requires eventual acceptance. Commits
remain enabled. The general safety instance makes no unconditional progress claim.
Two negative configurations must produce invariant violations: sequence-based identity
must violate `NoMixedGeneration`, and bypassed release must violate `NoReleasedRead`.
These checks do not prove C, transport delivery, output publication, or failure capture.

| Model action | Implementation and regression |
| --- | --- |
| CreatePin / Replace | `mo_protocol_export`, independently copied `mo_export_create`; `export_generations` creates twice at one sequence |
| Commit | existing service commit path; C trace reads old `a` bytes after `change`, Python verifies the entire old artifact |
| ReadPage | ID comparison before `mo_export_page_bounds` and copying, fixed offsets and count; CBMC checks exact arithmetic acceptance |
| Expire | `mo_export_expire`, poll deadline and page-time check; C fake clock expires the pin |
| Restart | supervisor restart and server free release the arena, new server starts empty; C stale-ID refusals for both |
| FetchAccept / final release | C final page release and Python SHA-256/coverage verification; U9 owns the actual fetcher and publication |

Development failure retained: the first release-negative-control run exposed an
unparenthesized TLA disjunction in the history-bit assignment, leaving a successor
variable unspecified. The runner rejected that error instead of treating any nonzero
exit as a detected invariant. Parenthesizing the expression yields the required
`NoReleasedRead` counterexample. The initial trace is retained in the local build logs.

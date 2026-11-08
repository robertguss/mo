# Frozen client catalog probe

The synthetic discovery/routing probe ran against the real GCC debug `mo serve`
with the final three upgrade-tool descriptions. The forwarding proxy does not
synthesize catalog entries or tool replies. No upgrade credential was configured;
this probe cannot upload or activate a candidate. These are development probes,
not the phase's agent evaluation.

```sh
CODEX_FROZEN=/home/exedev/.codex/packages/standalone/releases/0.166.0-x86_64-unknown-linux-musl/bin/codex \
CLAUDE_FROZEN=/home/exedev/.local/share/claude/versions/2.1.317 \
python3 learnability/phase5/preflight/remote/client_probe.py \
  build/gcc/debug/mo /tmp/mo-remote-clients codex codex-approve claude
```

All three processes exited zero; the probe's exact-version, visible-name,
raw-name and annotation assertions passed. The installed default executables
were newer (Codex 0.167.0 and Claude 2.1.319); the command deliberately used the
retained frozen binaries instead.

| Raw wire name | Codex 0.166.0 model-reported name | Claude Code 2.1.317 init-event name |
| --- | --- | --- |
| `mo-upgrade-open` | `mcp__probe__mo_upgrade_open` | `mcp__probe__mo-upgrade-open` |
| `mo-upgrade-page` | `mcp__probe__mo_upgrade_page` | `mcp__probe__mo-upgrade-page` |
| `mo-upgrade-commit` | `mcp__probe__mo_upgrade_commit` | `mcp__probe__mo-upgrade-commit` |

[client-summary.json](results/client-summary.json) pins the names and records
versions, actual commands, calls and catalog schemas/annotations.
[client-traffic.jsonl](results/client-traffic.jsonl) independently records wire
routing and the real catalog. All three have `readOnlyHint: false`; commit has
`destructiveHint: true`. Each description names `Mo-Upgrade-Authorization`.
The recorder stores authorization only as a boolean, never the bearer value.
Raw client streams remain outside the repository because they can contain local
hook output.

## Observations and limits

- Codex used `gpt-6.1-sol`, `xhigh`. Under never-approval, open and page reached
  the server, while commit was blocked locally with “MCP tool call requires
  approval, but approval policy is never”. Non-read-only annotations alone do
  not imply an approval block in this frozen client. Explicit tool approval
  allowed all three calls to reach the server.
- Claude discovered the hyphenated names and routed all three calls under the
  explicit allowed-tools setting. The requested model was `claude-opus-5-5`.
  Its own client emitted a safeguard refusal and automatically fell back to
  `claude-opus-4-8`; the curated fallback event is retained in the summary.
  This is evidence for the frozen client's discovery and routing, not a claim
  that Opus 5.5 alone completed the probe. No prompt was changed to circumvent
  the refusal and no fallback was requested by the probe runner.
- The probe deliberately passed `{}`. All calls that reached the server failed
  argument validation (`-32602`, `invalid view arguments`) before credential
  checking. Therefore these observations prove names and routing, not the
  credential-refusal contract; the remote driver separately tests that contract
  with valid-shaped arguments. No successful activation is claimed here.
- Codex visible names come from its final model report, as in Phase 4; Claude's
  come from its init event. Independent wire records establish the raw names.
- After this run, the recorder was extended to retain JSON-RPC error objects on
  future runs, and summary generation to retain client fallback events. The
  existing traffic file remains unchanged; its missing error fields are not
  reconstructed. The curated summary fallback came from the saved raw stream.

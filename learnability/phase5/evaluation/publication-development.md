# Publication staging controls

`publish.py` stages local artifacts only. Nothing is uploaded. Before invocation,
write a private JSON array containing the four session directories in declared
slot order, including every miss. Supply the collector's cumulative token file
(containing both service credentials for every slot) and provider secret file:

```
python3 learnability/phase5/evaluation/publish.py /PRIVATE/sessions.json /PRIVATE/publication --tokens /PRIVATE/state/tokens /PRIVATE/state/secrets
```

The destination must be absent. Only a completed `receipt.json` authorizes use of
its `evidence.tar.gz`: it records the archive hash, clean stage and extraction
scans, gate result and uploaded=false. An exception leaves partial local artifacts
for diagnosis; they are not approved for publication. The caller must supply all
credentials; the publisher cannot discover an omitted provider secret. At least
two distinct non-canary secret values are required. No credential is printed by
the scanner wrapper.

The tool requires all four correctly attributed manifests and rejects development
rows. A miss with absent stream/grade files remains present, with missing artifacts
listed; a claimed success requires stream and proxy files. It never invents a
successful hidden grade or closes the phase. The gate is the existing four-slot
aggregation. Original manifests, complete private logs and databases remain
unchanged. The curated archive contains manifest/grade when present, stream,
proxy, prompt, client stderr, service/restart logs, source-test output, and .mo
files from the collected task-queue and original serve folders. It excludes
SQLite databases, homes and credentials. Per-file inventories record both original
and curated hashes, and whether rendering/redaction changed bytes.

JSON/JSONL strings containing a raw or scanner-recognized encoded credential are
replaced entirely with a redaction marker; other fields remain. Text/source log
lines containing such values are replaced with marked lines. Credential-bearing
JSON keys or paths fail for manual curation. Entire-string/line replacement can
remove useful context; private originals retain it and the inventory flags changed
files. No hidden-test details are selectively removed after the experiment.

Phase 4 scanner patterns, escaped-byte decoding and file scanning are reused,
including its raw/base64/base64url/hex/prefix and URL/JSON escape coverage. Input
links and opaque files fail; archive extraction permits only relative regular files
and directories, rejects traversal/links/duplicate paths, and writes into a new
empty directory. Extracted bytes must exactly match the stage before the receipt.

Development validation (synthetic records only):
`python3 -m unittest discover -s learnability/phase5/evaluation -p test_publish.py -v`
passes four tests. Controls cover all four slots and a retained timeout miss;
service, upgrade and provider secrets in raw/base64/hex/URL/JSON-escaped strings;
unchanged private originals; missing slots and development exclusion; source
symlinks and traversal/absolute/symlink/hardlink archive attacks; and opaque data
refusal. No scored session, final archive or external publication was performed.

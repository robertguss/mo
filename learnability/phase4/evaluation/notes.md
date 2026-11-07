# Collection record

Robert authorized the 16-session collection against freeze commit `d4df49ef` on
2026-11-07 by answering “Yes” in this continuation conversation. The pre-collection
comparison passed for the installed binaries, all 269 build-input files, scripts,
protocol, private scenarios, round and order. The frozen protocol was not edited.

All 16 slots ran once in the seeded order. There were no exit-3 discards, exit-4
unstarted attempts, missing sessions, model substitutions or timeouts. Slots 15 and 16
are scored misses because their fixes failed hidden tests. Both received `verified`
from the separate fix checker after the original reproduced. HTTP connection-reset
exceptions occurred during those sessions; their client/proxy records still matched.
The evidence archive keeps the round log and all session streams and proxy logs.

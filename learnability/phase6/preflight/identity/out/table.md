Items: 19. Digest rule: canonical text (formatter output with comments and blank-line bits removed).

| Variant | `mo test queue-tests.mo` | Changed, signature included | Changed, signature left out |
| --- | --- | --- | --- |
| a-handler-fix | 47 passed, 0 failed, 0 stopped | 0 | 0 |
| b-release-fix | 47 passed, 0 failed, 0 stopped | 1: jobs.example.3 | 1: jobs.example.3 |
| c-max-attempts-4 | error E1405: a forbids anchor was accepted  job-queue.mo:185:5 | 2: fail.promise.2, jobs.example.3 | 2: fail.promise.2, jobs.example.3 |
| d-heartbeat-swap | 47 passed, 0 failed, 0 stopped | 2: heartbeat.promise.1, heartbeat.promise.2 | 0 |
| d2-fail-swap-main-caller-only | 41 passed, 6 failed, 0 stopped | 7: fail.promise.1, fail.promise.2, fail.example.1, fail.example.2, fail.example.3, fail.example.4, jobs.example.3 | 1: jobs.example.3 |
| d3-fail-swap-every-call | 47 passed, 0 failed, 0 stopped | 7: fail.promise.1, fail.promise.2, fail.example.1, fail.example.2, fail.example.3, fail.example.4, jobs.example.3 | 4: fail.example.2, fail.example.3, fail.example.4, jobs.example.3 |
| e-tool-rename | 47 passed, 0 failed, 0 stopped | 0 | 0 |
| g-release-comment | 47 passed, 0 failed, 0 stopped | 0 | 0 |
| h-layout-only | 47 passed, 0 failed, 0 stopped | 0 | 0 |

Changed item count by text rule (with signature / without signature):

| Variant | raw bytes | formatted, comments kept | canonical | reach lists changed |
| --- | --- | --- | --- | --- |
| a-handler-fix | 0 / 0 | 0 / 0 | 0 / 0 | 0 |
| b-release-fix | 1 / 1 | 1 / 1 | 1 / 1 | 0 |
| c-max-attempts-4 | 2 / 2 | 2 / 2 | 2 / 2 | 0 |
| d-heartbeat-swap | 2 / 0 | 2 / 0 | 2 / 0 | 0 |
| d2-fail-swap-main-caller-only | 7 / 1 | 7 / 1 | 7 / 1 | 0 |
| d3-fail-swap-every-call | 7 / 4 | 7 / 4 | 7 / 4 | 0 |
| e-tool-rename | 0 / 0 | 0 / 0 | 0 / 0 | 0 |
| g-release-comment | 1 / 1 | 1 / 1 | 0 / 0 | 0 |
| h-layout-only | 1 / 1 | 1 / 1 | 0 / 0 | 0 |

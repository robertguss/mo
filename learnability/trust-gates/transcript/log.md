# Gate log

## run-1 (full): 6 x E1440, service tools with no stated intent
Resolved each by adding an `examples` block in work/main.mo:
- Main.lease: 2 examples (empty queue gives None; one ready job is leased as `leased_job`)
- Main.heartbeat: 2 examples (current lease renews; wrong attempt is NotLeased)
- Main.acknowledge: 2 examples (current lease becomes `done_job`; done job is NotLeased)
- Main.fail: 2 examples (attempt 1 goes back to Ready; empty queue is NoSuchJob)
- Main.list: 3 examples (Done filter, Ready filter with no match, limit 0 is BadPage)
- Main.stats: 1 example (one leased job)

## run-2 (--against frozen): intent errors cleared; no mutants tried (all functions carried over)

## run-3 (full): 65 mutants tried, 59 killed, 6 survived
- `work/queue-tests.mo:26:30 QueueTests.job: int, 1 to 2`: JUDGED UNKILLABLE in its original form, because the helper kept only `.jobs.first`, which is the same job for page limit 1 or 2. Resolved by a code change to the test helper: it now returns the page's only job (`return found if page(...).jobs is [found]`, then `stop`), the same shape as the other helpers. A limit of 2 now stops every existing test that reads a job other than the last, so it is killed.
- `work/job-queue.mo:80:55 JobQueue.lease: int, 0 to 1`: new example `lease(empty, t0, 1.second) == Ok((empty, None))`.
- `work/job-queue.mo:253:78 JobQueue.holding: arithmetic, + to -`: new example on JobQueue.submit `submit(holding([ready(1)]), "x").1 == 2` (also `submit(empty, "x") == (holding([ready(1)]), 1)`).
- `work/job-queue.mo:253:80 JobQueue.holding: int, 1 to 2`: same new example as above, `submit(holding([ready(1)]), "x").1 == 2`.
- `work/job-queue.mo:257:39 JobQueue.ready: int, 0 to 1`: new example on JobQueue.submit `submit(empty, "x") == (holding([ready(1)]), 1)`.
- `work/job-queue.mo:265:75 JobQueue.tried: int, 30 to 31`: new example on JobQueue.lease `lease(holding([ready(1)]), t0, 30.seconds) == Ok((holding([tried(1, 1)]), Some(tried(1, 1))))`.

Judged unkillable: 1 (QueueTests.job limit). Killed only by one particular call sequence to the service: 0 (all new checks are pure examples; the helper rewrite is killed by many existing simulation tests).

## run-4 and run-5 (full): 65 mutants tried, 65 killed, 0 survived, 1 dropped; 67 passed, 0 failed, 0 stopped; exit status 0 (run-5)

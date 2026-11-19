The full gated run now passes with **exit status 0**. Last lines of the final run (`run-5.txt`):

```
gates: 65 mutants tried, 65 killed (1 promise, 35 example, 19 test, 0 recovery, 10 start), 0 survived, 1 dropped
67 passed, 0 failed, 0 stopped
exit status: 0
```

(`run-4.txt` gave the same result, but I hadn't captured its exit status, so I ran it again as `run-5.txt`.)

**Checks added: 16, all examples. No promises and no tests.**
- **12 examples in `work/main.mo`** for the 6 service tools the first run said stated no intent: lease 2, heartbeat 2, acknowledge 2, fail 2, list 3, stats 1. I used examples rather than a tool promise because `list` has a parameter named `after`, which the reference says can't be used in a tool promise.
- **4 examples in `work/job-queue.mo`** to kill 5 surviving mutants: 2 on `JobQueue.submit` and 2 on `JobQueue.lease`.

**Code changes: 1.** The test helper `QueueTests.job` only read `.jobs.first`, which is the same job whether the page limit is 1 or 2. So no check could kill the mutant that changes the limit `1` to `2`. I rewrote the helper to `return found if page(...).jobs is [found]`, followed by `stop`, which is the same shape as the other helpers. Strictly speaking this isn't the "simplification so the mutant no longer exists" the task describes. The mutant still exists, but the limit now matters and the existing simulation tests kill it. I've noted this in the log. The rewrite also removed the helper's `or`, so one fewer mutant is generated (dropped went from 2 to 1). No existing check was deleted or weakened, and every name and signature is unchanged.

**Counts from `log.md`:**
- Judged unkillable: **1** (the `QueueTests.job` limit mutant)
- Killed only by one particular call sequence to the service: **0**

The run output files `run-1.txt` to `run-5.txt` are all kept. `run-2.txt` is the `--against frozen` run; it tried no mutants because every function was carried over.

Nothing needed from you right now; the task is finished.

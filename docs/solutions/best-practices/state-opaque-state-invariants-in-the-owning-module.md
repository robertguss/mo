---
title: State an invariant of an opaque service state in its own module, not as a service promise
date: 2026-11-13
category: best-practices
module: service promises
problem_type: best_practice
component: testing_framework
severity: medium
applies_when:
  - A service's state type is exposed without its insides, such as `expose Queue` with no `(..)`
  - The guarantee you want to state is one the state's own module already keeps
  - Every value you can build for a `forbids` anchor satisfies the promise
tags: [service-promises, forbids, anchors, opaque-types, e0015, task-queue, job-queue]
---

# State an invariant of an opaque service state in its own module, not as a service promise

## Context

PR #337 added service promises and turned the task queue's guarantees into promises in
`services/task-queue/main.mo`. One of them, "no job is tried more than three times", could
not become a service promise. The reason is not obvious from the code that shipped: the
promise is simply missing, and nothing in the tree says why.

Every promise needs at least one `allows` and one `forbids` anchor
(`docs/reference/README.md:1185`), or `mo check` rejects it with E0015, "a promise has at least
one allows line and one forbids line". A service promise's anchors give values to `before`,
`after` and `now`, and those values have the service's state type, here `JobQueue.Queue`.
`JobQueue` exposes `Queue` without `(..)` (`services/task-queue/job-queue.mo:7`), so `Main` can
build a queue only by calling `JobQueue`'s exposed functions. Its existing anchors are built that
way (`one_job`, `leased_job` and `done_job`, `services/task-queue/main.mo:292-299`).

A `forbids` anchor for "no job is tried more than three times" needs a queue holding a job with
four or more attempts. `JobQueue`'s exposed functions never produce one, because `fail` and
`expire` both release a job through `release`, which marks it dead once it reaches
`max_attempts` (`services/task-queue/job-queue.mo:240`), and only `lease` adds an attempt. Without a `forbids` line the
promise fails with E0015; this was reproduced on the merged tree.

## Guidance

When a service promise has no `forbids` value you can build from outside the state's module,
the guarantee belongs to that module. State it there as a function promise, where the module's
private helpers can build the breaking value. `JobQueue` states the attempt limit on `fail` as
"a job is attempted at most three times" (`services/task-queue/job-queue.mo:181`), and its
`forbids` line uses the private helpers `holding` and `retried`
(`services/task-queue/job-queue.mo:252`, `:268`), which `Main` cannot call.

Keep service promises for guarantees about how calls, clock advances and restarts combine,
whose breaking states are built from public calls. Examples are "no job is ever lost" and "a
finished job stays as it finished", whose `forbids` anchors are an empty queue and a queue whose
job was never finished.

Do not expose the state's insides, or add a public constructor, only so that a `forbids` anchor
can be written. Doing so would give up the encapsulation that keeps the guarantee in the first
place.

## Why This Matters

If a value that breaks the promise could be built from the module's public calls, that value
would itself show a bug in the module. A missing `forbids` value is therefore a sign that the
module already keeps the guarantee for all outside code. The place to check it is the module's
own promises, which `mo test` runs on generated inputs.

Without this note, the next person who states guarantees for a service with an opaque state will
probably try the service promise, hit E0015, and then either give up on the guarantee or widen
the module's interface to get a `forbids` value. Neither is right.

## When to Apply

- When you write service promises for a service whose `state` type comes from another module
  and is exposed without `(..)`.
- When you are looking for a `forbids` value and every value you can build passes the promise.
- When you review a change that exposes a type's insides, or adds a constructor, and the only
  user of the new interface is a promise anchor.

## Examples

A promise that cannot be written, which is why `services/task-queue/main.mo` does not have it:

```mo
promise "no job is tried more than three times"
  JobQueue.jobs(after).all?(fn(job) job.attempts <= 3 end)
  allows before: one_job, after: one_job, now: t0
  # No `forbids` value: no exposed JobQueue call returns a job with 4 attempts.
end
```

`mo check` rejects this with E0015.

The promise where the guarantee lives instead, in `services/task-queue/job-queue.mo`, where
`holding` and `retried` are private:

```mo
promise "a job is attempted at most three times"
  result is Ok(after) and after.jobs.get(id) is Some(job) and job.attempts >= max_attempts implies
    job.state == Dead
  allows queue: holding([tried(1, 3)]), now: t0, id: 1, attempt: 3, result: Ok(holding([dead(1)]))
  forbids queue: holding([tried(1, 3)]), now: t0, id: 1, attempt: 3, result: Ok(
    holding([retried(1, 3)])
  )
end
```

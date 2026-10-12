/-
  examples/job-queue.mo in Core.

  Exercises: a state machine over values, maps keyed by `Int`, `find` with an
  anonymous function, `try` with `or_fail`, `is` patterns in conditions,
  promises about one step, and a test with a range loop and assertions.
-/
import MoCore.Testing

namespace MoCore.Examples.JobQueue

open MoCore DSL Lib

/-- `enum JobState  Ready  Leased(until: Time)  Done  Dead  end` -/
def JobState : DataDecl :=
  choice "JobState" [("Ready", []), ("Leased", [("until", tTime)]), ("Done", []), ("Dead", [])]

/-- `struct Job  id: Int  payload: Text  attempts: Int  state: JobState  end` -/
def Job : DataDecl :=
  record "Job" [("id", tInt), ("payload", tText), ("attempts", tInt), ("state", JobState.ty)]

/-- `struct Queue  jobs: Map(Int, Job)  next_id: Int  end` -/
def Queue : DataDecl := record "Queue" [("jobs", tMap tInt Job.ty), ("next_id", tInt)]

/-- `enum QueueError  NoSuchJob(id: Int)  NotLeased(id: Int)  BadLease(length: Duration)  end` -/
def QueueError : DataDecl :=
  choice "QueueError" [("NoSuchJob", [("id", tInt)]), ("NotLeased", [("id", tInt)]),
    ("BadLease", [("length", tDur)])]

def leasedTy : Ty := tTup [Queue.ty, tOpt Job.ty]
def leaseRes : Ty := tRes leasedTy QueueError.ty
def queueRes : Ty := tRes Queue.ty QueueError.ty
def jobs (e : Expr) : Expr := get Queue "jobs" e
def jmop (op : MapOp) (args : List Expr) : Expr := mop tInt Job.ty op args
def state (c : String) (args : List Expr := []) : Expr := mkC JobState c args
def seconds (n : Int) : Expr := iop .seconds [int n]

/-- `max_attempts = 3` -/
def maxAttempts : FunDecl := const "max_attempts" tInt (int 3)

/-- `empty = Queue(jobs: {}, next_id: 1)` -/
def emptyQ : FunDecl := const "empty" Queue.ty (mk Queue [emptyMap tInt Job.ty, int 1])

/-- `t0 = Time.parse("2026-10-12T09:00:00Z") or stop "the time is written correctly"` -/
def t0 : FunDecl :=
  const "t0" tTime <|
    orElse (prim (.time .parse) [str "2026-10-12T09:00:00Z"]) (stop "the time is written correctly")

/-- ```
fn submit(queue: Queue, payload: Text) : (Queue, Int)
  id = queue.next_id
  var after = queue
  after.jobs = after.jobs.set(id, Job(id: id, payload: payload, attempts: 0, state: Ready))
  after.next_id += 1
  (after, id)
end
``` -/
def submit : FunDecl :=
  fn "submit" [("queue", Queue.ty), ("payload", tText)] (tTup [Queue.ty, tInt]) <|
    lets [("id", get Queue "next_id" (v "queue")),
          ("after", v "queue"),
          ("after", setField Queue "jobs" (v "after")
            (jmop .set [jobs (v "after"), v "id", mk Job [v "id", v "payload", int 0, state "Ready"]])),
          ("after", setField Queue "next_id" (v "after") (iadd (get Queue "next_id" (v "after")) (int 1)))] <|
      tup [v "after", v "id"]

/-- ```
fn lease(queue: Queue, now: Time, length: Duration) : Result((Queue, Option(Job)), QueueError)
  return Error(BadLease(length: length)) if length <= 0.seconds
  until = try now.checked_add(length).or_fail(BadLease(length: length))
  case queue.jobs.values.find(fn(job) job.state == Ready end)
    None: Ok((queue, None))
    Some(job):
      var taken = job
      taken.state = Leased(until: until)
      taken.attempts += 1
      Ok((put(queue, taken), Some(taken)))
  end
end
``` -/
def lease : FunDecl :=
  fn "lease" [("queue", Queue.ty), ("now", tTime), ("length", tDur)] leaseRes <|
    ite (cmp .duration .le (v "length") (seconds 0))
      (err_ leasedTy QueueError.ty (mkC QueueError "BadLease" [v "length"])) <|
    tryRes leasedTy QueueError.ty
      (orFail tTime QueueError.ty (prim (.time .checkedAdd) [v "now", v "length"])
        (mkC QueueError "BadLease" [v "length"])) "until" <|
      caseOpt (findL Job.ty (jmop .values [jobs (v "queue")])
                (lam [("job", Job.ty)] (eq JobState.ty (get Job "state" (v "job")) (state "Ready")))) "job"
        (lets [("taken", v "job"),
               ("taken", setField Job "state" (v "taken") (state "Leased" [v "until"])),
               ("taken", setField Job "attempts" (v "taken") (iadd (get Job "attempts" (v "taken")) (int 1)))] <|
          ok_ leasedTy QueueError.ty (tup [call "put" [v "queue", v "taken"], some_ Job.ty (v "taken")]))
        (ok_ leasedTy QueueError.ty (tup [v "queue", none_ Job.ty]))

/-- ```
fn acknowledge(queue: Queue, id: Int) : Result(Queue, QueueError)
  job = try leased_job(queue, id)
  var finished = job
  finished.state = Done
  Ok(put(queue, finished))
end
``` -/
def acknowledge : FunDecl :=
  fn "acknowledge" [("queue", Queue.ty), ("id", tInt)] queueRes <|
    tryRes Queue.ty QueueError.ty (call "leased_job" [v "queue", v "id"]) "job" <|
      lets [("finished", v "job"), ("finished", setField Job "state" (v "finished") (state "Done"))] <|
        ok_ Queue.ty QueueError.ty (call "put" [v "queue", v "finished"])

/-- ```
fn fail(queue: Queue, id: Int) : Result(Queue, QueueError)
  job = try leased_job(queue, id)
  Ok(release(queue, job))
end
``` -/
def failFn : FunDecl :=
  fn "fail" [("queue", Queue.ty), ("id", tInt)] queueRes <|
    tryRes Queue.ty QueueError.ty (call "leased_job" [v "queue", v "id"]) "job" <|
      ok_ Queue.ty QueueError.ty (call "release" [v "queue", v "job"])

/-- ```
fn expire(queue: Queue, now: Time) : Queue
  var after = queue
  for job in queue.jobs.values
    if job.state is Leased(until: until) and until <= now
      after = release(after, job)
    end
  end
  after
end
``` -/
def expire : FunDecl :=
  fn "expire" [("queue", Queue.ty), ("now", tTime)] Queue.ty <|
    lets [("after", v "queue")] <|
      forList "x" Job.ty "job" (jmop .values [jobs (v "queue")]) [("after", Queue.ty)] [v "after"]
        (.case_ (get Job "state" (v "job"))
          [("Ready", [], nextRound "x" [v "after"]),
           ("Leased", ["until"],
             ite (cmp .time .le (v "until") (v "now"))
               (nextRound "x" [call "release" [v "after", v "job"]])
               (nextRound "x" [v "after"])),
           ("Done", [], nextRound "x" [v "after"]),
           ("Dead", [], nextRound "x" [v "after"])])
        (v "after")

/-- ```
fn leased_job(queue: Queue, id: Int) : Result(Job, QueueError)
  job = try queue.jobs.get(id).or_fail(NoSuchJob(id: id))
  case job.state
    Leased(until: _): Ok(job)
    Ready | Done | Dead: Error(NotLeased(id: id))
  end
end
``` -/
def leasedJob : FunDecl :=
  fn "leased_job" [("queue", Queue.ty), ("id", tInt)] (tRes Job.ty QueueError.ty) <|
    tryRes Job.ty QueueError.ty
      (orFail Job.ty QueueError.ty (jmop .get [jobs (v "queue"), v "id"]) (mkC QueueError "NoSuchJob" [v "id"]))
      "job" <|
      .case_ (get Job "state" (v "job"))
        [("Ready", [], err_ Job.ty QueueError.ty (mkC QueueError "NotLeased" [v "id"])),
         ("Leased", ["_"], ok_ Job.ty QueueError.ty (v "job")),
         ("Done", [], err_ Job.ty QueueError.ty (mkC QueueError "NotLeased" [v "id"])),
         ("Dead", [], err_ Job.ty QueueError.ty (mkC QueueError "NotLeased" [v "id"]))]

/-- ```
fn release(queue: Queue, job: Job) : Queue
  var released = job
  released.state = if job.attempts >= max_attempts: Dead else: Ready
  put(queue, released)
end
``` -/
def release : FunDecl :=
  fn "release" [("queue", Queue.ty), ("job", Job.ty)] Queue.ty <|
    lets [("released", v "job"),
          ("released", setField Job "state" (v "released")
            (ite (ige (get Job "attempts" (v "job")) (call "max_attempts" [])) (state "Dead") (state "Ready")))] <|
      call "put" [v "queue", v "released"]

/-- ```
fn put(queue: Queue, job: Job) : Queue
  var after = queue
  after.jobs = after.jobs.set(job.id, job)
  after
end
``` -/
def put : FunDecl :=
  fn "put" [("queue", Queue.ty), ("job", Job.ty)] Queue.ty <|
    lets [("after", v "queue"),
          ("after", setField Queue "jobs" (v "after") (jmop .set [jobs (v "after"), get Job "id" (v "job"), v "job"]))] <|
      v "after"

/-- ```
fn holding(jobs: List(Job)) : Queue
  Queue(jobs: jobs.map(fn(job) (job.id, job) end).to_map, next_id: jobs.size + 1)
end
``` -/
def holding : FunDecl :=
  fn "holding" [("jobs", tList Job.ty)] Queue.ty <|
    mk Queue [jmop .ofList [mapL Job.ty (tTup [tInt, Job.ty]) (v "jobs")
                (lam [("job", Job.ty)] (tup [get Job "id" (v "job"), v "job"]))],
              iadd (lop Job.ty .size [v "jobs"]) (int 1)]

/-- `fn ready(id: Int) : Job  Job(id: id, payload: "x", attempts: 0, state: Ready)  end`, and the
other helpers of the sample, each building one job. -/
def ready : FunDecl := fn "ready" [("id", tInt)] Job.ty <| mk Job [v "id", str "x", int 0, state "Ready"]
def leased : FunDecl :=
  fn "leased" [("id", tInt), ("until", tTime)] Job.ty <| mk Job [v "id", str "x", int 1, state "Leased" [v "until"]]
def tried : FunDecl :=
  fn "tried" [("id", tInt), ("attempts", tInt)] Job.ty <|
    mk Job [v "id", str "x", v "attempts", state "Leased" [call "t0" []]]
def retried : FunDecl :=
  fn "retried" [("id", tInt), ("attempts", tInt)] Job.ty <| mk Job [v "id", str "x", v "attempts", state "Ready"]
def done : FunDecl := fn "done" [("id", tInt)] Job.ty <| mk Job [v "id", str "x", int 1, state "Done"]
def dead : FunDecl := fn "dead" [("id", tInt)] Job.ty <| mk Job [v "id", str "x", int 3, state "Dead"]

/-- submit: "a submitted job is in the queue, ready"
`result.0.jobs.get(result.1) is Some(job) and job.state == Ready` -/
def submitReady : FunDecl :=
  fn "submit.promise.1" [("queue", Queue.ty), ("payload", tText), ("result", tTup [Queue.ty, tInt])] tBool <|
    caseOpt (jmop .get [jobs (fst (v "result")), snd (v "result")]) "job"
      (eq JobState.ty (get Job "state" (v "job")) (state "Ready")) ff

/-- fail: "a job is attempted at most three times"
`result is Ok(after) and after.jobs.get(id) is Some(job) and job.attempts >= max_attempts implies job.state == Dead` -/
def failAtMostThree : FunDecl :=
  fn "fail.promise.1" [("queue", Queue.ty), ("id", tInt), ("result", queueRes)] tBool <|
    caseRes (v "result")
      "after"
        (caseOpt (jmop .get [jobs (v "after"), v "id"]) "job"
          (implies (ige (get Job "attempts" (v "job")) (call "max_attempts" []))
            (eq JobState.ty (get Job "state" (v "job")) (state "Dead")))
          tt)
      "_" tt

def program : Program :=
  { data := [JobState, Job, Queue, QueueError]
    funs := [maxAttempts, emptyQ, t0, submit, lease, acknowledge, failFn, expire, leasedJob, release, put,
             holding, ready, leased, tried, retried, done, dead, submitReady, failAtMostThree,
             Lib.find Job.ty, Lib.map Job.ty (tTup [tInt, Job.ty])] }

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

def t0E : Expr := call "t0" []
def emptyE : Expr := call "empty" []
def holdingE (js : List Expr) : Expr := call "holding" [listOf Job.ty js]
def okQ (e : Expr) : Expr := ok_ Queue.ty QueueError.ty e
def errQ (c : String) (args : List Expr) : Expr := err_ Queue.ty QueueError.ty (mkC QueueError c args)

-- lease(empty, t0, 30.seconds) == Ok((empty, None))
#guard passes program
  (eq leaseRes (call "lease" [emptyE, t0E, seconds 30]) (ok_ leasedTy QueueError.ty (tup [emptyE, none_ Job.ty])))
-- lease(empty, t0, 0.seconds) == Error(BadLease(length: 0.seconds))
#guard passes program
  (eq leaseRes (call "lease" [emptyE, t0E, seconds 0])
    (err_ leasedTy QueueError.ty (mkC QueueError "BadLease" [seconds 0])))
-- fail(holding([tried(1, 1)]), 1) == Ok(holding([retried(1, 1)]))
#guard passes program
  (eq queueRes (call "fail" [holdingE [call "tried" [int 1, int 1]], int 1])
    (okQ (holdingE [call "retried" [int 1, int 1]])))
-- fail(holding([tried(1, 3)]), 1) == Ok(holding([dead(1)]))
#guard passes program
  (eq queueRes (call "fail" [holdingE [call "tried" [int 1, int 3]], int 1]) (okQ (holdingE [call "dead" [int 1]])))
-- fail(holding([ready(1)]), 1) == Error(NotLeased(id: 1))
#guard passes program (eq queueRes (call "fail" [holdingE [call "ready" [int 1]], int 1]) (errQ "NotLeased" [int 1]))
-- fail(empty, 9) == Error(NoSuchJob(id: 9))
#guard passes program (eq queueRes (call "fail" [emptyE, int 9]) (errQ "NoSuchJob" [int 9]))
-- expire(holding([leased(1, t0)]), t0) == holding([retried(1, 1)])
#guard passes program
  (eq Queue.ty (call "expire" [holdingE [call "leased" [int 1, t0E]], t0E]) (holdingE [call "retried" [int 1, int 1]]))
-- expire(holding([leased(1, t0 + 1.seconds)]), t0) == holding([leased(1, t0 + 1.seconds)])
#guard passes program
  (lets [("later", prim (.time .add) [t0E, seconds 1])] <|
    eq Queue.ty (call "expire" [holdingE [call "leased" [int 1, v "later"]], t0E])
      (holdingE [call "leased" [int 1, v "later"]]))

/-! ### The promises' anchors -/

-- allows queue: empty, payload: "x", result: (holding([ready(1)]), 1)
#guard passes program
  (call "submit.promise.1" [emptyE, str "x", tup [holdingE [call "ready" [int 1]], int 1]])
-- forbids queue: empty, payload: "x", result: (empty, 1)
#guard passes program (not_ (call "submit.promise.1" [emptyE, str "x", tup [emptyE, int 1]]))
-- allows queue: holding([tried(1, 3)]), id: 1, result: Ok(holding([dead(1)]))
#guard passes program
  (call "fail.promise.1" [holdingE [call "tried" [int 1, int 3]], int 1, okQ (holdingE [call "dead" [int 1]])])
-- forbids queue: holding([tried(1, 3)]), id: 1, result: Ok(holding([retried(1, 3)]))
#guard passes program
  (not_ (call "fail.promise.1" [holdingE [call "tried" [int 1, int 3]], int 1,
    okQ (holdingE [call "retried" [int 1, int 3]])]))

/-! ### Test "a job that keeps failing is tried three times, then dead"

```
(submitted, id) = submit(empty, "send the invoice")
var queue = submitted
for attempt in 1..4
  assert lease(queue, t0, 30.seconds) is Ok((leased_queue, Some(job)))
  assert job.attempts == attempt
  assert fail(leased_queue, id) is Ok(after)
  queue = after
end
assert queue.jobs.get(id) is Some(final)
assert final.state == Dead
assert lease(queue, t0, 30.seconds) == Ok((queue, None))
```
A failed `assert` stops the test. -/
#guard passes program
  (lets [("%s", call "submit" [emptyE, str "send the invoice"]),
         ("submitted", fst (v "%s")), ("id", snd (v "%s")), ("queue", v "submitted")] <|
    forRange "t" "attempt" (int 1) (int 4) [("queue", Queue.ty)] [v "queue"]
      (caseRes (call "lease" [v "queue", t0E, seconds 30])
        "%lq"
          (lets [("leased_queue", fst (v "%lq"))] <|
            caseOpt (snd (v "%lq")) "job"
              (assert (ieq (get Job "attempts" (v "job")) (v "attempt")) "job.attempts == attempt" <|
                caseRes (call "fail" [v "leased_queue", v "id"])
                  "after" (nextRange "t" "attempt" [v "after"])
                  "_" (stop "assertion failed: fail(leased_queue, id) is Ok(after)"))
              (stop "assertion failed: lease(queue, t0, 30.seconds) is Ok((leased_queue, Some(job)))"))
        "_" (stop "assertion failed: lease(queue, t0, 30.seconds) is Ok((leased_queue, Some(job)))"))
      (caseOpt (jmop .get [jobs (v "queue"), v "id"]) "final"
        (and_ (eq JobState.ty (get Job "state" (v "final")) (state "Dead"))
          (eq leaseRes (call "lease" [v "queue", t0E, seconds 30])
            (ok_ leasedTy QueueError.ty (tup [v "queue", none_ Job.ty]))))
        ff))

/-! ### Never wrong -/

/-- `lease` never goes wrong, for any queue, time and length. -/
theorem lease_never_wrong {ρ : Env}
    (hρ : EnvTy program ρ [("queue", Queue.ty), ("now", tTime), ("length", tDur)])
    (n : Nat) (w : World) (msg : String) :
    eval n program ρ w (call "lease" [v "queue", v "now", v "length"]) ≠ .wrong msg :=
  never_wrong (τ := leaseRes) checked (by decide +kernel) hρ n w msg

end MoCore.Examples.JobQueue

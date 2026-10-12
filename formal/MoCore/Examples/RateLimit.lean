/-
  examples/rate-limit.mo in Core.

  Exercises: time as a value (`Time - Time`, `in_seconds`, `+ Duration`),
  exact decimals (`min`, `max`, `ceil`, division), state as a value, an `if`
  with a value that is a tuple, and `stop` where a failure can't happen.
  Times count microseconds: `t0 + 500.milliseconds` is `t0 + 500000`.
-/
import MoCore.Testing

namespace MoCore.Examples.RateLimit

open MoCore DSL Lib

/-- `struct Bucket  tokens: Decimal  updated: Time  end` -/
def Bucket : DataDecl := record "Bucket" [("tokens", tDec), ("updated", tTime)]

/-- `struct Limiter  capacity: Decimal  per_second: Decimal  buckets: Map(Text, Bucket)  end` -/
def Limiter : DataDecl :=
  record "Limiter" [("capacity", tDec), ("per_second", tDec), ("buckets", tMap tText Bucket.ty)]

/-- `enum Decision  Allowed(remaining: Decimal)  Refused(retry_after: Duration)  end` -/
def Decision : DataDecl :=
  choice "Decision" [("Allowed", [("remaining", tDec)]), ("Refused", [("retry_after", tDur)])]

def stepTy : Ty := tTup [Limiter.ty, Decision.ty]
def tokens (e : Expr) : Expr := get Bucket "tokens" e

/-- `t0 = Time.parse("2026-10-12T09:00:00Z") or stop "the time is written correctly"` -/
def t0 : FunDecl :=
  const "t0" tTime <|
    orElse (prim (.time .parse) [str "2026-10-12T09:00:00Z"]) (stop "the time is written correctly")

/-- ```
fn new_limiter(capacity: Decimal, per_second: Decimal) : Option(Limiter)
  return None if capacity < 1 or capacity > 1_000_000_000
  return None if per_second < 0.001 or per_second > 1_000_000
  Some(Limiter(capacity: capacity, per_second: per_second, buckets: {}))
end
``` -/
def newLimiter : FunDecl :=
  fn "new_limiter" [("capacity", tDec), ("per_second", tDec)] (tOpt Limiter.ty) <|
    ite (or_ (dlt (v "capacity") (dec "1")) (dgt (v "capacity") (dec "1000000000"))) (none_ Limiter.ty) <|
    ite (or_ (dlt (v "per_second") (dec "0.001")) (dgt (v "per_second") (dec "1000000"))) (none_ Limiter.ty) <|
      some_ Limiter.ty (mk Limiter [v "capacity", v "per_second", emptyMap tText Bucket.ty])

/-- ```
fn request(limiter: Limiter, client: Text, now: Time) : (Limiter, Decision)
  full = Bucket(tokens: limiter.capacity, updated: now)
  bucket = refill(limiter.buckets.get(client) or full, limiter, now)
  (kept, decision) = if bucket.tokens >= 1
    (Bucket(tokens: bucket.tokens - 1, updated: now), Allowed(remaining: bucket.tokens - 1))
  else
    (bucket, Refused(retry_after: wait_for_one(bucket.tokens, limiter.per_second)))
  end
  var after = limiter
  after.buckets = after.buckets.set(client, kept)
  (after, decision)
end
``` -/
def request : FunDecl :=
  fn "request" [("limiter", Limiter.ty), ("client", tText), ("now", tTime)] stepTy <|
    lets [("full", mk Bucket [get Limiter "capacity" (v "limiter"), v "now"]),
          ("bucket", call "refill"
            [orElse (mop tText Bucket.ty .get [get Limiter "buckets" (v "limiter"), v "client"]) (v "full"),
             v "limiter", v "now"]),
          ("%kd", ite (dge (tokens (v "bucket")) (dec "1"))
            (tup [mk Bucket [dsub (tokens (v "bucket")) (dec "1"), v "now"],
                  mkC Decision "Allowed" [dsub (tokens (v "bucket")) (dec "1")]])
            (tup [v "bucket",
                  mkC Decision "Refused"
                    [call "wait_for_one" [tokens (v "bucket"), get Limiter "per_second" (v "limiter")]]])),
          ("kept", fst (v "%kd")), ("decision", snd (v "%kd")),
          ("after", v "limiter"),
          ("after", setField Limiter "buckets" (v "after")
            (mop tText Bucket.ty .set [get Limiter "buckets" (v "after"), v "client", v "kept"]))] <|
      tup [v "after", v "decision"]

/-- ```
fn refill(bucket: Bucket, limiter: Limiter, now: Time) : Bucket
  elapsed = (now - bucket.updated).in_seconds.max(0)
  tokens = (bucket.tokens + elapsed * limiter.per_second).min(limiter.capacity)
  Bucket(tokens: tokens, updated: now)
end
``` -/
def refill : FunDecl :=
  fn "refill" [("bucket", Bucket.ty), ("limiter", Limiter.ty), ("now", tTime)] Bucket.ty <|
    lets [("elapsed", dop .max [prim (.time .inSeconds) [prim (.time .sub) [v "now", get Bucket "updated" (v "bucket")]],
                                dec "0"]),
          ("tokens", dop .min [dadd (tokens (v "bucket")) (dmul (v "elapsed") (get Limiter "per_second" (v "limiter"))),
                               get Limiter "capacity" (v "limiter")])] <|
      mk Bucket [v "tokens", v "now"]

/-- ```
fn wait_for_one(tokens: Decimal, per_second: Decimal) : Duration
  wait = ((1 - tokens) * 1000 / per_second).ceil
  case wait.to_int
    Some(n): n.milliseconds
    None: stop "the rate is at least 0.001 a second, so the wait is at most 10^6 milliseconds"
  end
end
``` -/
def waitForOne : FunDecl :=
  fn "wait_for_one" [("tokens", tDec), ("per_second", tDec)] tDur <|
    lets [("wait", dop .ceil [ddiv (dmul (dsub (dec "1") (v "tokens")) (dec "1000")) (v "per_second")])] <|
      caseOpt (dop .toInt [v "wait"]) "n"
        (iop .milliseconds [v "n"])
        (stop "the rate is at least 0.001 a second, so the wait is at most 10^6 milliseconds")

/-- `fn limiter_of(capacity: Decimal) : Limiter  Limiter(capacity: capacity, per_second: 2, buckets: {})  end` -/
def limiterOf : FunDecl :=
  fn "limiter_of" [("capacity", tDec)] Limiter.ty <|
    mk Limiter [v "capacity", dec "2", emptyMap tText Bucket.ty]

/-- request: "a client is never allowed more tokens than its bucket holds"
`result.1 is Allowed(remaining: left) implies left >= 0 and left <= limiter.capacity - 1` -/
def requestBounded : FunDecl :=
  fn "request.promise.1" [("limiter", Limiter.ty), ("client", tText), ("now", tTime), ("result", stepTy)] tBool <|
    .case_ (snd (v "result"))
      [("Allowed", ["left"],
         and_ (dge (v "left") (dec "0"))
           (dle (v "left") (dsub (get Limiter "capacity" (v "limiter")) (dec "1")))),
       ("Refused", ["_"], tt)]

def program : Program :=
  { data := [Bucket, Limiter, Decision]
    funs := [t0, newLimiter, request, refill, waitForOne, limiterOf, requestBounded] }

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

/-! ### Examples -/

-- new_limiter(10, 2) is Some(_)
#guard passes program (isSome (call "new_limiter" [dec "10", dec "2"]))
-- new_limiter(0.5, 2) == None
#guard passes program (eq (tOpt Limiter.ty) (call "new_limiter" [dec "0.5", dec "2"]) (none_ Limiter.ty))
-- new_limiter(10, 0) == None
#guard passes program (eq (tOpt Limiter.ty) (call "new_limiter" [dec "10", dec "0"]) (none_ Limiter.ty))

/-! ### The promise's anchors -/

def t0E : Expr := call "t0" []
def allowed (s : String) : Expr := mkC Decision "Allowed" [dec s]
-- allows limiter: limiter_of(2), client: "a", now: t0, result: (limiter_of(2), Allowed(remaining: 1))
#guard passes program
  (call "request.promise.1" [call "limiter_of" [dec "2"], str "a", t0E, tup [call "limiter_of" [dec "2"], allowed "1"]])
-- forbids ... result: (limiter_of(2), Allowed(remaining: 2))
#guard passes program
  (not_ (call "request.promise.1" [call "limiter_of" [dec "2"], str "a", t0E,
    tup [call "limiter_of" [dec "2"], allowed "2"]]))

/-! ### Test "a bucket of two allows two requests, then refills in half a second"

```
assert new_limiter(2, 2) is Some(limiter)
(after_one, first) = request(limiter, "ana", t0)
(after_two, second) = request(after_one, "ana", t0)
(after_three, third) = request(after_two, "ana", t0)
assert first == Allowed(remaining: 1)
assert second == Allowed(remaining: 0)
assert third == Refused(retry_after: 500.milliseconds)
(_, later) = request(after_three, "ana", t0 + 500.milliseconds)
assert later == Allowed(remaining: 0)
``` -/
#guard passes program
  (caseOpt (call "new_limiter" [dec "2", dec "2"]) "limiter"
    (lets [("%1", call "request" [v "limiter", str "ana", t0E]),
           ("after_one", fst (v "%1")), ("first", snd (v "%1")),
           ("%2", call "request" [v "after_one", str "ana", t0E]),
           ("after_two", fst (v "%2")), ("second", snd (v "%2")),
           ("%3", call "request" [v "after_two", str "ana", t0E]),
           ("after_three", fst (v "%3")), ("third", snd (v "%3"))] <|
      and_ (eq Decision.ty (v "first") (allowed "1")) <|
      and_ (eq Decision.ty (v "second") (allowed "0")) <|
      and_ (eq Decision.ty (v "third") (mkC Decision "Refused" [iop .milliseconds [int 500]])) <|
      lets [("later", snd (call "request"
              [v "after_three", str "ana", prim (.time .add) [t0E, iop .milliseconds [int 500]]]))] <|
        eq Decision.ty (v "later") (allowed "0"))
    ff)

/-! ### Test "each client has its own bucket"

```
assert new_limiter(1, 1) is Some(limiter)
(after_ana, _) = request(limiter, "ana", t0)
(_, for_ben) = request(after_ana, "ben", t0)
assert for_ben == Allowed(remaining: 0)
``` -/
#guard passes program
  (caseOpt (call "new_limiter" [dec "1", dec "1"]) "limiter"
    (lets [("after_ana", fst (call "request" [v "limiter", str "ana", t0E])),
           ("for_ben", snd (call "request" [v "after_ana", str "ben", t0E]))] <|
      eq Decision.ty (v "for_ben") (allowed "0"))
    ff)

/-! ### More: the promise holds along a longer request sequence, with a slow refill -/

#guard passes program
  (caseOpt (call "new_limiter" [dec "3", dec "0.5"]) "limiter"
    (lets [("%1", call "request" [v "limiter", str "ana", t0E]),
           ("%2", call "request" [fst (v "%1"), str "ana", t0E]),
           ("%3", call "request" [fst (v "%2"), str "ana", t0E]),
           ("%4", call "request" [fst (v "%3"), str "ana", t0E]),
           ("%5", call "request" [fst (v "%4"), str "ana", prim (.time .add) [t0E, iop .seconds [int 1]]])] <|
      and_ (call "request.promise.1" [v "limiter", str "ana", t0E, v "%1"]) <|
      and_ (eq Decision.ty (snd (v "%3")) (allowed "0")) <|
      -- with no token left and 0.5 tokens a second, the next one comes in 2 seconds
      and_ (eq Decision.ty (snd (v "%4")) (mkC Decision "Refused" [iop .milliseconds [int 2000]])) <|
      -- one second later, half a token: refused for another second
        eq Decision.ty (snd (v "%5")) (mkC Decision "Refused" [iop .milliseconds [int 1000]]))
    ff)

/-! ### Never wrong -/

/-- `request` never goes wrong, for any limiter, client and time. -/
theorem request_never_wrong {ρ : Env}
    (hρ : EnvTy program ρ [("limiter", Limiter.ty), ("client", tText), ("now", tTime)])
    (n : Nat) (w : World) (msg : String) :
    eval n program ρ w (call "request" [v "limiter", v "client", v "now"]) ≠ .wrong msg :=
  never_wrong (τ := stepTy) checked (by decide +kernel) hρ n w msg

end MoCore.Examples.RateLimit

---------------------------- MODULE Capture ----------------------------
EXTENDS Naturals, FiniteSets
CONSTANTS Bug, Kinds, MaxFailures, MaxRecords, MaxApply
VARIABLES phase, failures, kind, counted, safe, app, log, atFail,
          pending, doomed, rows, recs, published, lost
vars == <<phase, failures, kind, counted, safe, app, log, atFail,
          pending, doomed, rows, recs, published, lost>>
store == <<rows, recs, published, lost>>

\* recs holds <<id, kind, first>>; rows holds the two halves <<id, 1>> and <<id, 2>>.
Halves(ids) == ids \X {1, 2}
Ids(rs) == {r[1] : r \in rs}
OfKind(k) == {r \in recs : r[2] = k}
Latest(k) == CHOOSE i \in Ids(OfKind(k)) : \A j \in Ids(OfKind(k)) : j <= i
Repeat == OfKind(kind) # {}
Own == Ids({r \in OfKind(kind) : ~r[3]})
Room == Cardinality(recs) - Cardinality(Own) < MaxRecords
Others == {r[2] : r \in recs} \ {kind}
Oldest == CHOOSE k \in Others : \A j \in Others : Latest(k) <= Latest(j)
Applied == /\ rows' = (rows \ Halves(doomed)) \cup Halves({failures})
           /\ recs' = {r \in recs : r[1] \notin doomed} \cup {<<failures, kind, ~Repeat>>}
           /\ lost' = (lost \/ \E r \in recs : r[1] \in doomed /\ r[2] # kind /\ Repeat)

Init == /\ phase = "idle" /\ failures = 0 /\ kind = CHOOSE k \in Kinds : TRUE
        /\ counted = 0 /\ safe = TRUE /\ app = 0 /\ log = 0 /\ atFail = <<0, 0>>
        /\ pending = {} /\ doomed = {} /\ rows = {} /\ recs = {} /\ published = {}
        /\ lost = FALSE

\* Successful handlers: the only actions that change application state or the log.
Apply == /\ phase = "idle" /\ safe /\ app < MaxApply
         /\ app' = app + 1 /\ log' = log + 1
         /\ UNCHANGED <<phase, failures, kind, counted, safe, atFail, pending, doomed, store>>
Snapshot == /\ phase = "idle" /\ safe /\ log > 0 /\ log' = 0
            /\ UNCHANGED <<phase, failures, kind, counted, safe, app, atFail, pending, doomed,
                           store>>
Fail == /\ phase = "idle" /\ safe /\ failures < MaxFailures
        /\ \E k \in Kinds : kind' = k
        /\ failures' = failures + 1 /\ atFail' = <<app, log>> /\ phase' = "prepare"
        /\ UNCHANGED <<counted, safe, app, log, pending, doomed, store>>
Prepare == /\ phase = "prepare" /\ phase' = "retain"
           /\ UNCHANGED <<failures, kind, counted, safe, app, log, atFail, pending, doomed, store>>
PrepareRefuse == /\ phase = "prepare" /\ phase' = "count"
                 /\ UNCHANGED <<failures, kind, counted, safe, app, log, atFail, pending, doomed,
                                store>>
\* A repeat deletes only its kind's previous latest. A new kind may start with eviction.
Retain == /\ phase = "retain" /\ (Room \/ (Bug = "bound" /\ ~Repeat))
          /\ doomed' = Own /\ phase' = "write"
          /\ UNCHANGED <<failures, kind, counted, safe, app, log, atFail, pending, store>>
Evict == /\ phase = "retain" /\ ~Room /\ (~Repeat \/ Bug = "repeat") /\ Bug # "bound"
         /\ Others # {} /\ doomed' = Own \cup Ids(OfKind(Oldest)) /\ phase' = "write"
         /\ UNCHANGED <<failures, kind, counted, safe, app, log, atFail, pending, store>>
RetainRefuse == /\ phase = "retain" /\ ~Room /\ Repeat /\ Bug # "repeat" /\ phase' = "count"
                /\ UNCHANGED <<failures, kind, counted, safe, app, log, atFail, pending, doomed,
                               store>>
Write == /\ phase = "write" /\ pending' = Halves({failures}) /\ phase' = "commit"
         /\ published' = IF Bug = "publish" THEN published \cup {failures} ELSE published
         /\ UNCHANGED <<failures, kind, counted, safe, app, log, atFail, doomed, rows, recs,
                        lost>>
Commit == /\ phase = "commit" /\ Applied
          /\ published' = (published \ doomed) \cup {failures}
          /\ log' = IF Bug = "recovery" THEN log + 1 ELSE log
          /\ pending' = {} /\ doomed' = {} /\ phase' = "count"
          /\ UNCHANGED <<failures, kind, counted, safe, app, atFail>>
\* COMMIT reported an error: the pair may or may not be durable, and no id is published.
CommitUncertain == /\ phase = "commit" /\ safe' = FALSE /\ phase' = "count"
                   /\ pending' = {} /\ doomed' = {}
                   /\ \/ Applied /\ published' = published \ doomed
                      \/ UNCHANGED store
                   /\ UNCHANGED <<failures, kind, counted, app, log, atFail>>
Rollback == /\ phase \in {"write", "commit"} /\ pending' = {} /\ doomed' = {}
            /\ phase' = "count"
            /\ counted' = IF Bug = "count" THEN counted + 1 ELSE counted
            /\ UNCHANGED <<failures, kind, safe, app, log, atFail, store>>
RollbackFail == /\ phase \in {"write", "commit"} /\ safe' = FALSE /\ phase' = "count"
                /\ pending' = {} /\ doomed' = {}
                /\ UNCHANGED <<failures, kind, counted, app, log, atFail, store>>
Crash == /\ phase \in {"write", "commit", "count"} /\ pending' = {} /\ doomed' = {}
         /\ phase' = "recover"
         /\ UNCHANGED <<failures, kind, counted, safe, app, log, atFail, store>>
Recover == /\ phase = "recover" /\ phase' = "count"
           /\ UNCHANGED <<failures, kind, counted, safe, app, log, atFail, pending, doomed, store>>
Count == /\ phase = "count" /\ counted' = counted + 1
         /\ phase' = IF safe \/ Bug = "unsafe" THEN "idle" ELSE "stopped"
         /\ UNCHANGED <<failures, kind, safe, app, log, atFail, pending, doomed, store>>
\* The operator restarts a stopped service; reopening may find an uncertain pair.
Reopen == /\ phase = "stopped" /\ phase' = "idle" /\ safe' = TRUE
          /\ UNCHANGED <<failures, kind, counted, app, log, atFail, pending, doomed, store>>
Next == Apply \/ Snapshot \/ Fail \/ Prepare \/ PrepareRefuse \/ Retain \/ Evict
        \/ RetainRefuse \/ Write \/ Commit \/ CommitUncertain \/ Rollback \/ RollbackFail
        \/ Crash \/ Recover \/ Count \/ Reopen
Spec == Init /\ [][Next]_vars

PairComplete == \A id \in published \cup Ids(recs) : Halves({id}) \subseteq rows
RecoveryUntouched == phase # "idle" => <<app, log>> = atFail
CountedOnce == counted <= failures /\ (phase \in {"idle", "stopped"} => counted = failures)
NoUnsafeContinue == ~safe => phase \in {"count", "recover", "stopped"}
Retention == /\ Cardinality(recs) <= MaxRecords
             /\ \A k \in Kinds : OfKind(k) # {} =>
                    /\ Cardinality(OfKind(k)) <= 2
                    /\ Cardinality({r \in OfKind(k) : r[3]}) = 1
RepeatKeepsOthers == ~lost
=============================================================================

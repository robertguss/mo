---------------------------- MODULE Capture ----------------------------
EXTENDS Naturals, FiniteSets
CONSTANTS EarlyPublish, DoubleCount
VARIABLES phase, pending, durable, published, failures, counted, safe, app, log
vars == <<phase, pending, durable, published, failures, counted, safe, app, log>>
Init == /\ phase = "idle" /\ pending = {} /\ durable = {} /\ published = {}
        /\ failures = 0 /\ counted = 0 /\ safe = TRUE /\ app = 7 /\ log = 3
Fail == /\ phase = "idle" /\ failures < 2 /\ safe
        /\ failures' = failures + 1 /\ phase' = "prepare"
        /\ UNCHANGED <<pending,durable,published,counted,safe,app,log>>
Prepare == /\ phase = "prepare" /\ phase' = "begin"
           /\ UNCHANGED <<pending,durable,published,failures,counted,safe,app,log>>
PrepareRefuse == /\ phase = "prepare" /\ phase' = "count"
                 /\ UNCHANGED <<pending,durable,published,failures,counted,safe,app,log>>
Begin == /\ phase = "begin" /\ phase' = "write"
         /\ UNCHANGED <<pending,durable,published,failures,counted,safe,app,log>>
Write == /\ phase = "write" /\ pending' = {<<failures,1>>,<<failures,2>>}
         /\ phase' = "commit"
         /\ published' = IF EarlyPublish THEN published \cup {failures} ELSE published
         /\ UNCHANGED <<durable,failures,counted,safe,app,log>>
Commit == /\ phase = "commit" /\ durable' = durable \cup pending /\ pending' = {}
          /\ published' = published \cup {failures} /\ phase' = "count"
          /\ UNCHANGED <<failures,counted,safe,app,log>>
Rollback == /\ phase \in {"write","commit"} /\ pending' = {} /\ phase' = "count"
            /\ counted' = IF DoubleCount THEN counted + 1 ELSE counted
            /\ UNCHANGED <<durable,published,failures,safe,app,log>>
Unsafe == /\ phase \in {"write","commit"} /\ safe' = FALSE /\ phase' = "count"
          /\ pending' = {} /\ UNCHANGED <<durable,published,failures,counted,app,log>>
Crash == /\ phase \in {"write","commit"} /\ pending' = {} /\ phase' = "recover"
         /\ UNCHANGED <<durable,published,failures,counted,safe,app,log>>
Recover == /\ phase = "recover" /\ phase' = "count"
           /\ UNCHANGED <<pending,durable,published,failures,counted,safe,app,log>>
Count == /\ phase = "count" /\ counted' = counted + 1
         /\ phase' = IF safe THEN "idle" ELSE "stopped"
         /\ UNCHANGED <<pending,durable,published,failures,safe,app,log>>
Evict == /\ phase = "idle" /\ 1 \in published
         /\ durable' = durable \ {<<1,1>>,<<1,2>>} /\ published' = published \ {1}
         /\ UNCHANGED <<phase,pending,failures,counted,safe,app,log>>
Next == Fail \/ Prepare \/ PrepareRefuse \/ Begin \/ Write \/ Commit \/ Rollback \/ Unsafe \/ Crash \/ Recover \/ Count \/ Evict
Spec == Init /\ [][Next]_vars
PairComplete == \A id \in published : {<<id,1>>,<<id,2>>} \subseteq durable
RecoveryUntouched == app = 7 /\ log = 3
CountedOnce == counted <= failures /\ (phase \in {"idle","stopped"} => counted = failures)
NoUnsafeContinue == ~safe => phase \in {"count","stopped"}
=============================================================================

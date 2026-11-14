----------------------------- MODULE Swap -----------------------------
EXTENDS Naturals, Sequences, FiniteSets
CONSTANTS Bug, Door, Programs, Messages, MaxUpgrades, MaxApprovals
VARIABLES prog, ord, upgrades, snap, snapKey, tail,
          setting, approved, aver, pending,
          phase, live, uncertain, mailbox, unsent, slot,
          submitted, acked, removed, done
durable == <<prog, ord, upgrades, snap, snapKey, tail>>
door == <<setting, approved, aver, pending>>
volatile == <<phase, live, uncertain, mailbox, unsent, slot>>
client == <<submitted, acked, removed>>
vars == <<durable, door, volatile, client, done>>

\* Message 2 calls a tool that program 2 removes and program 3 restores.
Tool(m) == IF m = 2 THEN "b" ELSE "a"
Has(p, m) == Tool(m) = "a" \/ p # 2
\* Program 2 edits item q into q2; program 3 keeps q and adds r.
Items(p) == CASE p = 1 -> {"p", "q"} [] p = 2 -> {"p", "q2"} [] OTHER -> {"p", "q", "r"}
Queued == {mailbox[i] : i \in DOMAIN mailbox}
\* A review id binds the base, the candidate, the migration and the approval-set version.
NoRid == <<0, 0, FALSE, 0>>
Rids == Programs \X Programs \X BOOLEAN \X (0..(MaxApprovals + MaxUpgrades))
NoSlot == [st |-> "none", base |-> 0, cand |-> 0, mig |-> FALSE, aver |-> 0, how |-> "none",
           rid |-> NoRid]
\* Restart restores snap and replays the tail; History is what that replay applies.
History == snap \o [i \in DOMAIN tail |-> tail[i][1]]
Count(m) == Cardinality({i \in DOMAIN History : History[i] = m})
\* The negative control for conservation answers removed tools before the outcome is known.
Early == IF Bug = "conservation" THEN {m \in Queued : ~Has(slot.cand, m)} ELSE {}
Keeps(c) == approved \subseteq Items(c)
\* The negative control for the yes matches the pending upgrade on its candidate alone.
Names(id) == IF Bug = "rid" THEN pending # NoRid /\ id[2] = pending[2] ELSE pending = id
\* The negative control for the version lets a commit through after an approval changed.
Current == slot.base = prog /\ (Bug = "version" \/ slot.aver = aver)

Init == /\ prog = 1 /\ ord = 0 /\ upgrades = <<>> /\ snap = <<>> /\ snapKey = 1
        /\ tail = <<>> /\ setting = "everything" /\ approved = {} /\ aver = 0
        /\ pending = NoRid /\ phase = "up" /\ live = 1 /\ uncertain = FALSE
        /\ mailbox = <<>> /\ unsent = {} /\ slot = NoSlot
        /\ submitted = {} /\ acked = {} /\ removed = {} /\ done = <<>>

Submit == /\ phase = "up" /\ \E m \in Messages \ submitted :
                /\ mailbox' = Append(mailbox, m) /\ submitted' = submitted \cup {m}
          /\ UNCHANGED <<durable, door, phase, live, uncertain, unsent, slot, acked, removed,
                         done>>
\* One message per step: apply under the live program and append its record in one commit.
Handle == /\ phase = "up" /\ mailbox # <<>> /\ Has(live, Head(mailbox))
          /\ tail' = Append(tail, <<Head(mailbox), live>>)
          /\ mailbox' = Tail(mailbox) /\ unsent' = unsent \cup {Head(mailbox)}
          /\ UNCHANGED <<prog, ord, upgrades, snap, snapKey, door, phase, live, uncertain, slot,
                         client, done>>
\* The live program lacks the tool: error reply, no record, no effect.
HandleRemoved == /\ phase = "up" /\ mailbox # <<>> /\ ~Has(live, Head(mailbox))
                 /\ mailbox' = Tail(mailbox) /\ removed' = removed \cup {Head(mailbox)}
                 /\ UNCHANGED <<durable, door, phase, live, uncertain, unsent, slot, submitted,
                                acked, done>>
Reply == /\ phase = "up" /\ \E m \in unsent : unsent' = unsent \ {m} /\ acked' = acked \cup {m}
         /\ UNCHANGED <<durable, door, phase, live, uncertain, mailbox, slot, submitted, removed,
                        done>>
Snapshot == /\ phase = "up" /\ tail # <<>> /\ snap' = History /\ tail' = <<>>
            /\ UNCHANGED <<prog, ord, upgrades, snapKey, door, volatile, client, done>>
\* An upload opens only on the active base; an already-active candidate or a stale base is
\* answered without any change, so those pushes are stuttering steps. A busy slot refuses.
Open == /\ phase = "up" /\ slot.st = "none" /\ ord < MaxUpgrades
        /\ \E c \in Programs \ {prog}, m \in IF Door THEN BOOLEAN ELSE {FALSE} :
              slot' = [NoSlot EXCEPT !.st = "open", !.base = prog, !.cand = c, !.mig = m]
        /\ UNCHANGED <<durable, door, phase, live, uncertain, mailbox, unsent, client, done>>
\* The last page arrives, the candidate compiles and passes its checks, and its items are listed.
Stage == /\ phase = "up" /\ slot.st = "open" /\ slot' = [slot EXCEPT !.st = "staged"]
         /\ UNCHANGED <<durable, door, phase, live, uncertain, mailbox, unsent, client, done>>
\* A compile error, a failed check or an unfinished plan refuses the push.
StageRefuse == /\ phase = "up" /\ slot.st = "open" /\ slot' = NoSlot
               /\ UNCHANGED <<durable, door, phase, live, uncertain, mailbox, unsent, client,
                              done>>
\* The decision reads the active program, the setting and the approvals at this moment. Without
\* the door every staged candidate activates, as in Phase 5.
Decide == /\ phase = "up" /\ slot.st = "staged" /\ slot.base = prog
          /\ IF ~Door \/ (setting = "intent" /\ Keeps(slot.cand) /\ ~slot.mig)
             THEN /\ slot' = [slot EXCEPT !.st = "commit", !.aver = aver, !.how = "auto"]
                  /\ UNCHANGED pending
             \* A held push replaces any pending upgrade and releases the slot.
             ELSE /\ pending' = <<prog, slot.cand, slot.mig, aver>> /\ slot' = NoSlot
          /\ UNCHANGED <<durable, setting, approved, aver, phase, live, uncertain, mailbox,
                         unsent, client, done>>
\* Robert answers yes with any review id; only one naming the pending upgrade starts a recheck.
Yes == /\ phase = "up" /\ slot.st = "none" /\ \E id \in Rids :
            /\ Names(id)
            /\ slot' = [st |-> "recheck", base |-> pending[1], cand |-> pending[2],
                        mig |-> pending[3], aver |-> pending[4], how |-> "yes", rid |-> id]
       /\ UNCHANGED <<durable, door, phase, live, uncertain, mailbox, unsent, client, done>>
Recheck == /\ phase = "up" /\ slot.st = "recheck" /\ slot' = [slot EXCEPT !.st = "commit"]
           /\ UNCHANGED <<durable, door, phase, live, uncertain, mailbox, unsent, client, done>>
\* The stored candidate fails on this build: refused after a yes, and the pending upgrade dropped.
RecheckRefuse == /\ phase = "up" /\ slot.st = "recheck" /\ slot' = NoSlot /\ pending' = NoRid
                 /\ UNCHANGED <<durable, setting, approved, aver, phase, live, uncertain,
                                mailbox, unsent, client, done>>
No == /\ phase = "up" /\ \E id \in Rids : pending = id /\ pending # NoRid
      /\ pending' = NoRid
      /\ UNCHANGED <<durable, setting, approved, aver, volatile, client, done>>
\* A first approval of an item of the active program; any write to approvals makes the pending
\* upgrade stale.
Approve == /\ phase = "up" /\ aver < MaxApprovals /\ \E i \in Items(prog) \ approved :
                 approved' = approved \cup {i}
           /\ aver' = aver + 1 /\ pending' = NoRid
           /\ UNCHANGED <<durable, setting, volatile, client, done>>
\* A later change of the setting never decides an upgrade already held.
Setting == /\ Door /\ phase = "up" /\ setting' = IF setting = "intent" THEN "everything" ELSE "intent"
           /\ UNCHANGED <<durable, approved, aver, pending, volatile, client, done>>
\* The base or the approval-set version moved before the boundary: nothing activates, and a yes
\* in flight is recorded as refused and its pending upgrade dropped.
Stale == /\ phase = "up" /\ slot.st = "commit" /\ ~Current /\ slot' = NoSlot
         /\ IF slot.how = "yes" THEN pending' = NoRid ELSE UNCHANGED pending
         /\ UNCHANGED <<durable, setting, approved, aver, phase, live, uncertain, mailbox, unsent,
                        client, done>>
Boundary == phase = "up" /\ slot.st = "commit" /\ Current /\ unsent = {}
\* A yes approves the items the review showed, edited and added; every activation retires the
\* approvals the candidate lacks.
Granted == IF slot.how = "yes" THEN (approved \cap Items(slot.cand)) \cup
                                    (Items(slot.cand) \ Items(prog))
           ELSE approved \cap Items(slot.cand)
\* The activation transaction: program row, ordinal, upgrade record, migrated snapshot at the
\* head under the candidate's key, tail deleted, approvals and the review record; the upgrade
\* record retains the previous program, and a pending upgrade is dropped as stale.
Activated == /\ prog' = slot.cand /\ ord' = ord + 1
             /\ upgrades' = Append(upgrades, <<ord + 1, prog, slot.cand>>)
             /\ snapKey' = slot.cand
             \* The negative control migrates the old snapshot and keeps the old tail to replay.
             /\ IF Bug = "cross" THEN UNCHANGED <<snap, tail>>
                ELSE snap' = History /\ tail' = <<>>
             /\ approved' = Granted /\ aver' = IF Granted = approved THEN aver ELSE aver + 1
             /\ pending' = NoRid
             /\ done' = Append(done, [how |-> slot.how, rid |-> slot.rid, mig |-> slot.mig,
                                      keeps |-> Keeps(slot.cand),
                                      bound |-> <<prog, slot.cand, slot.mig, aver>>])
\* Queued messages stay in order ahead of later arrivals; removed tools are answered by Handle.
Committed == /\ Boundary /\ Activated /\ live' = slot.cand /\ slot' = NoSlot
             /\ removed' = removed \cup Early
             /\ UNCHANGED <<setting, phase, uncertain, mailbox, unsent, submitted, acked>>
Refused == /\ Boundary /\ slot' = NoSlot /\ removed' = removed \cup Early
           /\ UNCHANGED <<durable, door, phase, live, uncertain, mailbox, unsent, submitted,
                          acked, done>>
\* COMMIT reported an error: either store is possible and the service stops.
Uncertain == /\ Boundary /\ uncertain' = TRUE /\ removed' = removed \cup Early
             /\ \/ Activated
                \/ UNCHANGED <<durable, approved, aver, pending, done>>
             /\ IF Bug = "authority"
                THEN UNCHANGED <<phase, live, mailbox, unsent>> /\ slot' = NoSlot
                ELSE phase' = "down" /\ mailbox' = <<>> /\ unsent' = {} /\ slot' = NoSlot
                     /\ live' = live
             /\ UNCHANGED <<setting, submitted, acked>>
Crash == /\ phase = "up" /\ phase' = "down" /\ mailbox' = <<>> /\ unsent' = {}
         /\ slot' = NoSlot
         /\ UNCHANGED <<durable, door, live, uncertain, client, done>>
\* Restart reads the program row and keeps the pending upgrade; the migration is never rerun.
Restart == /\ phase = "down" /\ phase' = "up" /\ live' = prog /\ uncertain' = FALSE
           /\ UNCHANGED <<durable, door, mailbox, unsent, slot, client, done>>
Next == Submit \/ Handle \/ HandleRemoved \/ Reply \/ Snapshot \/ Open \/ Stage
        \/ StageRefuse \/ Decide \/ Yes \/ Recheck \/ RecheckRefuse \/ No \/ Approve
        \/ Setting \/ Stale \/ Committed \/ Refused \/ Uncertain \/ Crash \/ Restart
Spec == Init /\ [][Next]_vars

SingleAuthority == /\ phase = "up" => live = prog
                   /\ uncertain => phase = "down"
                   /\ ord = Len(upgrades)
                   /\ ord > 0 => upgrades[ord][3] = prog
NoCrossUpgradeRecord == /\ snapKey = prog
                        /\ \A i \in DOMAIN tail : tail[i][2] = prog
MessageConservation == /\ \A m \in Messages : Count(m) <= 1
                       /\ \A m \in acked : Count(m) = 1
                       /\ \A m \in removed : Count(m) = 0
\* No candidate that changes approved intent, or names a migration, activates without a yes.
NoUnreviewedIntent == \A i \in DOMAIN done :
                          done[i].how = "auto" => done[i].keeps /\ ~done[i].mig
\* A yes activates only the candidate its review id names, on the base and approvals it binds.
YesNamesCandidate == \A i \in DOMAIN done : done[i].how = "yes" => done[i].rid = done[i].bound
=============================================================================

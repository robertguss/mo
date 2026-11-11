----------------------------- MODULE Swap -----------------------------
EXTENDS Naturals, Sequences, FiniteSets
CONSTANTS Bug, Programs, Messages, MaxUpgrades
VARIABLES prog, ord, upgrades, snap, snapKey, tail,
          phase, live, uncertain, mailbox, unsent, slot,
          submitted, acked, removed
durable == <<prog, ord, upgrades, snap, snapKey, tail>>
volatile == <<phase, live, uncertain, mailbox, unsent, slot>>
client == <<submitted, acked, removed>>
vars == <<durable, volatile, client>>

\* Message 2 calls a tool that program 2 removes and program 3 restores.
Tool(m) == IF m = 2 THEN "b" ELSE "a"
Has(p, m) == Tool(m) = "a" \/ p # 2
Queued == {mailbox[i] : i \in DOMAIN mailbox}
NoSlot == [st |-> "none", base |-> 0, cand |-> 0]
\* Restart restores snap and replays the tail; History is what that replay applies.
History == snap \o [i \in DOMAIN tail |-> tail[i][1]]
Count(m) == Cardinality({i \in DOMAIN History : History[i] = m})
\* The negative control for conservation answers removed tools before the outcome is known.
Early == IF Bug = "conservation" THEN {m \in Queued : ~Has(slot.cand, m)} ELSE {}

Init == /\ prog = 1 /\ ord = 0 /\ upgrades = <<>> /\ snap = <<>> /\ snapKey = 1
        /\ tail = <<>> /\ phase = "up" /\ live = 1 /\ uncertain = FALSE
        /\ mailbox = <<>> /\ unsent = {} /\ slot = NoSlot
        /\ submitted = {} /\ acked = {} /\ removed = {}

Submit == /\ phase = "up" /\ \E m \in Messages \ submitted :
                /\ mailbox' = Append(mailbox, m) /\ submitted' = submitted \cup {m}
          /\ UNCHANGED <<durable, phase, live, uncertain, unsent, slot, acked, removed>>
\* One message per step: apply under the live program and append its record in one commit.
Handle == /\ phase = "up" /\ mailbox # <<>> /\ Has(live, Head(mailbox))
          /\ tail' = Append(tail, <<Head(mailbox), live>>)
          /\ mailbox' = Tail(mailbox) /\ unsent' = unsent \cup {Head(mailbox)}
          /\ UNCHANGED <<prog, ord, upgrades, snap, snapKey, phase, live, uncertain, slot,
                         client>>
\* The live program lacks the tool: error reply, no record, no effect.
HandleRemoved == /\ phase = "up" /\ mailbox # <<>> /\ ~Has(live, Head(mailbox))
                 /\ mailbox' = Tail(mailbox) /\ removed' = removed \cup {Head(mailbox)}
                 /\ UNCHANGED <<durable, phase, live, uncertain, unsent, slot, submitted,
                                acked>>
Reply == /\ phase = "up" /\ \E m \in unsent : unsent' = unsent \ {m} /\ acked' = acked \cup {m}
         /\ UNCHANGED <<durable, phase, live, uncertain, mailbox, slot, submitted, removed>>
Snapshot == /\ phase = "up" /\ tail # <<>> /\ snap' = History /\ tail' = <<>>
            /\ UNCHANGED <<prog, ord, upgrades, snapKey, volatile, client>>
\* An upload opens only on the active base; an already-active candidate or a stale base is
\* answered without any change, so those pushes are stuttering steps.
Open == /\ phase = "up" /\ slot.st = "none" /\ ord < MaxUpgrades
        /\ \E c \in Programs \ {prog} : slot' = [st |-> "open", base |-> prog, cand |-> c]
        /\ UNCHANGED <<durable, phase, live, uncertain, mailbox, unsent, client>>
Stage == /\ phase = "up" /\ slot.st = "open" /\ slot' = [slot EXCEPT !.st = "staged"]
         /\ UNCHANGED <<durable, phase, live, uncertain, mailbox, unsent, client>>
StageRefuse == /\ phase = "up" /\ slot.st = "open" /\ slot' = NoSlot
               /\ UNCHANGED <<durable, phase, live, uncertain, mailbox, unsent, client>>
CommitRequest == /\ phase = "up" /\ slot.st = "staged" /\ slot.base = prog
                 /\ slot' = [slot EXCEPT !.st = "commit"]
                 /\ UNCHANGED <<durable, phase, live, uncertain, mailbox, unsent, client>>
Boundary == phase = "up" /\ slot.st = "commit" /\ slot.base = prog /\ unsent = {}
\* The activation transaction: program row, ordinal, upgrade record, migrated snapshot at the
\* head under the candidate's key, tail deleted; the upgrade record retains the previous program.
Activated == /\ prog' = slot.cand /\ ord' = ord + 1
             /\ upgrades' = Append(upgrades, <<ord + 1, prog, slot.cand>>)
             /\ snapKey' = slot.cand
             \* The negative control migrates the old snapshot and keeps the old tail to replay.
             /\ IF Bug = "cross" THEN UNCHANGED <<snap, tail>>
                ELSE snap' = History /\ tail' = <<>>
\* Queued messages stay in order ahead of later arrivals; removed tools are answered by Handle.
Committed == /\ Boundary /\ Activated /\ live' = slot.cand /\ slot' = NoSlot
             /\ removed' = removed \cup Early
             /\ UNCHANGED <<phase, uncertain, mailbox, unsent, submitted, acked>>
Refused == /\ Boundary /\ slot' = NoSlot /\ removed' = removed \cup Early
           /\ UNCHANGED <<durable, phase, live, uncertain, mailbox, unsent, submitted, acked>>
\* COMMIT reported an error: either store is possible and the service stops.
Uncertain == /\ Boundary /\ uncertain' = TRUE /\ removed' = removed \cup Early
             /\ \/ Activated
                \/ UNCHANGED durable
             /\ IF Bug = "authority"
                THEN UNCHANGED <<phase, live, mailbox, unsent>> /\ slot' = NoSlot
                ELSE phase' = "down" /\ mailbox' = <<>> /\ unsent' = {} /\ slot' = NoSlot
                     /\ live' = live
             /\ UNCHANGED <<submitted, acked>>
Crash == /\ phase = "up" /\ phase' = "down" /\ mailbox' = <<>> /\ unsent' = {}
         /\ slot' = NoSlot
         /\ UNCHANGED <<durable, live, uncertain, client>>
\* Restart reads the program row; the migration is never rerun.
Restart == /\ phase = "down" /\ phase' = "up" /\ live' = prog /\ uncertain' = FALSE
           /\ UNCHANGED <<durable, mailbox, unsent, slot, client>>
Next == Submit \/ Handle \/ HandleRemoved \/ Reply \/ Snapshot \/ Open \/ Stage
        \/ StageRefuse \/ CommitRequest \/ Committed \/ Refused \/ Uncertain \/ Crash
        \/ Restart
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
=============================================================================

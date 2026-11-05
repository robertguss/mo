---------------------------- MODULE Export ----------------------------
EXTENDS Naturals, Sequences, TLC
CONSTANTS BrokenGeneration, BrokenRelease, Disruptions
VARIABLES sequence, state, pin, nextId, restarts, fetchId, fetchSeq,
          expected, pages, accepted, releasedRead
vars == <<sequence, state, pin, nextId, restarts, fetchId, fetchSeq,
          expected, pages, accepted, releasedRead>>
Empty == [id |-> 0, seq |-> 0, bytes |-> <<0, 0>>, released |-> TRUE]
Init == /\ sequence = 0 /\ state = 0 /\ pin = Empty /\ nextId = 1
        /\ restarts = 0 /\ fetchId = 0 /\ fetchSeq = 0 /\ expected = <<>>
        /\ pages = <<>> /\ accepted = FALSE /\ releasedRead = FALSE
Commit == /\ sequence < 2
          /\ sequence' = sequence + 1 /\ state' = state + 1
          /\ UNCHANGED <<pin, nextId, restarts, fetchId, fetchSeq, expected,
                          pages, accepted, releasedRead>>
NewPin == /\ nextId <= 3
          /\ pin' = [id |-> nextId, seq |-> sequence,
                     bytes |-> <<state, state>>, released |-> FALSE]
          /\ nextId' = nextId + 1
          /\ IF fetchId = 0 THEN
                  /\ fetchId' = nextId /\ fetchSeq' = sequence
                  /\ expected' = <<state, state>>
             ELSE UNCHANGED <<fetchId, fetchSeq, expected>>
          /\ UNCHANGED <<sequence, state, restarts, pages, accepted, releasedRead>>
CreatePin == /\ fetchId = 0 /\ NewPin
Replace == /\ Disruptions /\ fetchId # 0 /\ NewPin
ReadPage == /\ fetchId # 0 /\ Len(pages) < 2 /\ pin.id # 0
            /\ (~pin.released \/ BrokenRelease)
            /\ IF BrokenGeneration THEN pin.seq = fetchSeq ELSE pin.id = fetchId
            /\ pages' = Append(pages, [id |-> pin.id, byte |-> pin.bytes[Len(pages)+1]])
            /\ pin' = [pin EXCEPT !.released = (Len(pages) = 1)]
            /\ releasedRead' = (releasedRead \/ pin.released)
            /\ UNCHANGED <<sequence, state, nextId, restarts, fetchId,
                            fetchSeq, expected, accepted>>
Expire == /\ Disruptions /\ pin.id # 0 /\ ~pin.released
          /\ pin' = [pin EXCEPT !.released = TRUE]
          /\ UNCHANGED <<sequence, state, nextId, restarts, fetchId, fetchSeq,
                          expected, pages, accepted, releasedRead>>
Restart == /\ Disruptions /\ restarts < 2
           /\ restarts' = restarts + 1 /\ pin' = Empty
           /\ UNCHANGED <<sequence, state, nextId, fetchId, fetchSeq, expected,
                           pages, accepted, releasedRead>>
FetchAccept == /\ Len(pages) = 2 /\ ~accepted /\ accepted' = TRUE
               /\ UNCHANGED <<sequence, state, pin, nextId, restarts, fetchId,
                               fetchSeq, expected, pages, releasedRead>>
Next == Commit \/ CreatePin \/ ReadPage \/ Replace \/ Expire \/ Restart \/ FetchAccept
Spec == Init /\ [][Next]_vars
FairSpec == Spec /\ WF_vars(CreatePin) /\ WF_vars(ReadPage) /\ WF_vars(FetchAccept)
NoMixedGeneration == \A i \in 1..Len(pages):
                       pages[i].id = fetchId /\ pages[i].byte = expected[i]
NoReleasedRead == ~releasedRead
Completes == <>accepted
=============================================================================

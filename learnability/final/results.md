# The final round: results

The final round tested the reference at local commit 9d24831, with decision
D227 written in (`notes.md`). Each family wrote the twelve sample tasks and
the four held-out tasks, one fresh session per task. Two graders, Claude Opus
5.5 (A) and Claude Fable 5.1 (B), graded each family independently against
the reference and the rubric. A third grader settled the one program they
disagreed on.

| Family | Parse | Grader A | Grader B | Verdicts |
| --- | --- | --- | --- | --- |
| OpenAI: GPT-6.1 Sol | 16 of 16 | 14 pass, 2 close | 14 pass, 2 close | 14 pass, 2 close (todo, library) |
| Anthropic: Claude Code | 16 of 16 | 16 pass | 15 pass, 1 close | 15 pass, 1 close (job-queue, settled by the third grader) |

## Against Phase 1's targets

| Target | Needed | Result |
| --- | --- | --- |
| Attempts that parse | at least 95% | 32 of 32: 100% |
| Sample tasks that pass | at least 75% | 22 of 24: 92% |
| Sample tasks each family passes | at least 8 of 12 | GPT-6.1 Sol 11, Claude Code 11 |
| Sample tasks that pass or come close | at least 90% | 24 of 24: 100% |
| Held-out tasks that pass | at least 65% | 7 of 8: 88% |
| Held-out tasks that pass or come close | at least 85% | 8 of 8: 100% |

Every target is met.

## Stumbles

Three mistakes in 32 programs, each made once, by one family. No rule
tripped two families.

1. **`x.to_text` on a type that section 14 gives no `to_text`** (GPT-6.1 Sol,
   todo: `cause.to_text` on a `FileError`). The reference lists `to_text` for
   some built-in types, and other values reach their text form through
   `"#{x}"`. The session assumed every type with a text form has `to_text`.
2. **`Time - Duration`** (GPT-6.1 Sol, library: `anchor_time - 1.seconds`).
   Section 3.9 allows a time minus a time, and a time plus a duration, but not
   a time minus a duration.
3. **A positional pattern for a case with one named field** (Claude Code,
   job-queue: `BadLength(_)` for `BadLength(length: Duration)`). Section 7.3
   gives the positional form only for a case's one unnamed value. The
   reference's own prose in section 14 wrote `Error(NotAllowed(path))` for a
   named-field case, which may have invited the slip.

The third is fixed in the reference: section 14 now writes
`Error(NotAllowed(path: path))`, and section 7.3 says that a case with named
fields is matched by naming them, even when there is only one. The first two
would be small changes a user would notice, so they go to Robert.

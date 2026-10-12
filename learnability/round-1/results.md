# Round 1: results

Round 1 is diagnostic (see the protocol). Each family wrote the twelve sample
programs from the reference alone. Two graders, Claude Opus 5.5 (A) and Claude
Fable 5.1 (B), graded each family independently against the reference and
the rubric. A third grader looks only at programs where they disagree.

| Family | Sessions | Parse | Grader A | Grader B | Verdicts |
| --- | --- | --- | --- | --- | --- |
| OpenAI: GPT-6.1 Sol | one session for all twelve | 12 of 12 | 12 pass | 12 pass | 12 pass |
| Anthropic: Claude Code | one session per program | 12 of 12 | 12 pass | 12 pass | 12 pass |

**Stumbles: none.** Neither grader, in either family, found a mistake that a
compiler following the reference would reject, or a program that doesn't do
its task. The graders agree on every program, so no third grader was needed.
Round 1 asks for no change to the language. The graders listed the forms that came
closest, each allowed by a sentence of the reference:

- a bare number taking its type from its context, such as `amount * 10` for a
  `BigInt` (3.3);
- local names and fields that share a name with a library or module function
  (5.4, 6.1);
- the same name bound in separate blocks, such as two arms of a `case` (6.1);
- `try` wrapping an error through the one case that holds it (8.3);
- a statement after a `case` arm's colon, such as an assignment (appendix B).

**How it ran** is in `notes.md`. Claude Code needed one session per program:
a single session reading the reference and planning all twelve programs
ended each time without writing a file, apparently at its output limit. Two
of the per-program sessions failed the same way and were run again, with a
sentence asking for a first version of the file early. Round 1 is
diagnostic, so these differences are recorded, not corrected. The final round
uses one fresh session per task and family anyway.

**What the final round needs:** the reference with the gaps filled that
building Mo Core and the architecture found (one decision for Robert), then
fresh sessions for the twelve tasks and the four held-out tasks, two families,
32 sessions in all.

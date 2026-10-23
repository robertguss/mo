# The learnability test

Can fresh models write correct Mo from the Language Reference alone? Phase 1
answered this with two model families, OpenAI's GPT-6.1 Sol and Anthropic's
Claude through Claude Code (decision D226), before the reference was approved.

- [`rubric.md`](rubric.md): how every program is graded. A program **passes**
  when it parses, has no mistake a compiler following the reference would
  reject, and does its task; it **comes close** when the task is done with at
  most two such mistakes.
- [`round-1/`](round-1/): the diagnostic round, the twelve sample tasks.
  Both families passed all twelve. See [`round-1/results.md`](round-1/results.md).
- [`final/`](final/): the final round, the twelve sample tasks and four
  held-out tasks, with one fresh session per task and family. It met every
  target of Phase 1. See [`final/results.md`](final/results.md).

Phase 2's rounds (the three tools, benchmark v0 and the first-retry measure)
follow [`protocol.md`](protocol.md): the `mo` command and hidden tests grade,
blind Claude and non-Claude graders check the tests, Robert checks the graders
on 30 programs, and every rate carries its Wilson interval
([`wilson.py`](wilson.py)), and [`retry.py`](retry.py) turns a round's session logs into
the first-retry numbers its report gives. Each session runs `mo` through the session-recording
harness ([`harness/`](harness/README.md)), which keeps every run for `tally` and `notes.md`.

Each round's folder keeps the exact prompts, every program as written, both
graders' gradings, and `notes.md`, a log of how the round ran.

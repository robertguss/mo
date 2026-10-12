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

Each round's folder keeps the exact prompts, every program as written, both
graders' gradings, and `notes.md`, a log of how the round ran.

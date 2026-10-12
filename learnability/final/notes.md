# The final round: how it ran

The final round tests the reference as it will be approved: `docs/reference/README.md`
at local commit 9d24831, which includes decision D227 (SHA-256
a204e5dda610558b2d1d58041f44a3247a8737009d33060407647c1eb6cdd27b). Each
session downloaded it from a temporary link, checked to give exactly that
file.

- 32 fresh sessions, started 12 Oct 2026 at about 10:47 UTC: one per task and
  family, for the twelve sample tasks and the four held-out tasks
  (`tasks.md`). Anthropic: Claude Code with its default model. OpenAI: Amp's
  GPT-6.1 Sol mode. Thread IDs are in `threads.tsv`.
- Every session got the same prompt, `prompt.md`, with only its task and file
  name filled in. It includes the procedural sentence that round 1 needed for
  Claude Code: write a first version of the file soon, then improve it.
- The sessions aren't archived when they finish; each file is collected with
  `thread_file_url`, and the thread archived afterwards.
- All 32 sessions wrote their files, between 10:49 and 11:09 UTC. None ended without a file. GPT-6.1 Sol took 1 to 4 minutes per program, and Claude Code 2 to 21.
- A finished session's orb pauses after a while, and then `thread_file_url` can't reach it. So each session was sent one message after it had finished: "This is the test's coordinator. Your file is being collected now. Don't change any file or run anything: reply only with the word OK." All 32 replied only "OK", with no tool calls, and the files were then collected. All 32 threads are archived.
- The executable grammar parses all 32 files.
- Graders A and B agree on all of GPT-6.1 Sol's programs: 14 pass, and todo and library close. For Claude Code they disagree on job-queue only (A pass, B close). A third grader, Claude Opus 5.5 in a fresh session, settled it as close (`grade-c-job-queue.md`). Results are in `results.md`.

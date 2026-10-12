# Round 1: how it ran

Round 1 is diagnostic (see the protocol). One session per family wrote all twelve programs from the frozen prompt in `prompt.md`, with the reference downloaded from a private link.

## OpenAI: GPT-6.1 Sol

- Amp thread in the `gpt61s` mode, started 12 Oct 2026 about 01:02 UTC.
- It wrote the twelve files, sent their links back as the prompt asks, and the files were copied to `gpt-6.1-sol/`.
- The executable grammar parses all twelve.

## Anthropic: Claude Code

- Amp thread running Claude Code with its default model, started 12 Oct 2026 at 01:02 UTC.
- Its first turn downloaded and read the reference, then ended at about 01:21 UTC with no text and no files, most likely because its one response grew past the output limit.
- At 01:26 UTC it was sent one procedural message, with nothing about Mo in it: continue with step 2, writing one file per response; don't download the reference again; if `thread_file_url` or `send_thread_message` isn't available, stop after writing the files, and they will be collected.
- The message never reached the thread. Its state stayed "streaming", with no change after 01:21 UTC, and it wrote no files, so at 01:34 UTC it was archived without answers.
- A second Claude Code thread was started at 01:33 UTC. Its prompt is in `prompt-claude-code-retry.md`: the frozen prompt plus two procedural sentences and nothing about Mo. The first asks for one file per response. The second says to stop after writing the files if the tools for sending them back aren't available. This second session is the Anthropic participant of record, whatever happens to the first.
- The second session failed in the same way. It read the reference at 01:34 UTC, and at 01:52 UTC ended its turn with no text and no files. It was archived.
- From 01:55 UTC, the Anthropic participant is twelve fresh Claude Code sessions, one per program. Each gets the prompt in `prompt-per-task.md`: the frozen prompt's wording, with only its own task. That is the shape the protocol already uses for the final round. The sessions' thread IDs are in `claude-code-threads.tsv`. OpenAI's round 1 stays one session for all twelve programs. Round 1 is diagnostic, so the difference is recorded here, not corrected.
- Each per-program session archives itself when it finishes, which stops its orb. To collect a file, the thread is unarchived, the file fetched with `thread_file_url`, and the thread archived again.
- GPT-6.1 Sol's grading: graders A and B each passed all twelve programs. They agree, so no third grader was needed.
- Ten of the twelve per-program sessions wrote their files. Those for rate-limit and job-queue ended at about 02:13 UTC with an empty response and no file, as the single sessions had. At 02:16 UTC each got a second, fresh session. The prompt was the per-task prompt plus one procedural sentence: write a first version of the file soon, then improve it with edits, so that no single response grows too long.
- Both second attempts wrote their files by 02:32 UTC, working in many small edits. All twelve of Claude Code's answers parse.
- Claude Code's grading: grader A passed all twelve. Grader B's first run ended at its output limit before writing anything. It was rerun at 03:15 UTC, with the instruction to grade one program at a time and append each grading to its file.
- Grader B's rerun passed all twelve of Claude Code's programs, as grader A did. The graders agree on all 24 programs, so no third grader was needed.

# The three tools' diagnostic round: how it ran

- Frozen at local commit 77755f5 (`freeze.md`), on mo f27cac3.
- 24 Oct 2026, about 01:33 UTC: six fresh sessions started, one per tool per family, each an Amp thread with no project, so no repository checkout and no project instructions. Thread IDs are in `threads.tsv`. Every session got `prompt.md` with only its package link, file name and task filled in.
- The package link is temporary and expires at 04:30 UTC; a session that hasn't installed `mo` by then is one that failed before seeing its task only if it also hasn't read the task, which every prompt includes, so it would be graded as it stands.

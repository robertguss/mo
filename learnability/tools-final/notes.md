# The three tools' final round: how it ran

- Frozen at commit 3b26805 on branch `mo-63-final-round` (`freeze.md`), on mo 522c4c8.
- The package was tested in a fresh orb with no project before freezing: its hash matched, setup printed `ready`, the recording `mo` was on `PATH`, the embedded guide had MO-115's rule, a `Unicode16` program checked and ran, and `session.py check-log` accepted the log in `/home/user/workspace/mo-session`.
- 24 Oct 2026, about 11:19 UTC: six fresh sessions started, one per tool per family, each an Amp thread with no project, so no repository checkout and no project instructions. Thread IDs are in `threads.tsv`. Every session got `prompt.md` with only its package link, file name and task filled in. The package link expires at 14:09 UTC; every session installs `mo` in its first step.

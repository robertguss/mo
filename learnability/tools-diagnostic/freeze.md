# The three tools' diagnostic round: freeze

Frozen 24 Oct 2026, before any session saw a task (MO-63, protocol section 1). This is a diagnostic round: its findings improve the guide and messages, and its results are reported as their own round.

## The toolchain

- `mo` commit: f27cac36e8c372b6e2bd0f3e7abbb8889be7a2f8 (main after #174).
- Built with `make CC=gcc build/gcc/debug/mo` (gcc 12.2.0, Debian 12) from a clean clone at `/home/user/.mo-frozen`, so the binary's built-in `lib/` path is `/home/user/.mo-frozen/lib`, the same path in every session orb.
- Binary SHA-256: `665265b6f06b6b793f45e91c788b5d9162605dcb4ecce108fe33f83da45c6308`. The harness refuses any other binary, and grading uses this one.
- Reference (`docs/reference/README.md`) SHA-256: `b910a44ae1c281d6218729d8301dd17eff0be7aa5a96f306b6efe59ad66ccebf`.
- Guide (`docs/agent-guide.md`) SHA-256: `043ec88b36846c87601e54dff6265266fad9c03c57ed74b9cda377cabe096575`.
- Sessions read both only through `mo guide` and `mo guide --reference`, which print the copies embedded in the binary.
- Session package `mo63-session.tgz`, SHA-256 `39f535fec9ada44065391a22c464ea7625429086f179025a63c3d2094784f223`: the binary, the commit's `lib/`, `learnability/harness/session.py`, and `setup.sh`, which runs `session.py start` with the hash above, work folder `/home/user/workspace/mo-work` and session folder `/home/user/.mo-session`, and puts the recording `mo` on `PATH`. Tested in a fresh orb on 24 Oct before freezing.

## The prompt

`prompt.md`, word for word. `PACKAGE_URL` is the package's temporary download link, `NAME` the tool's name, and `TASK` the first paragraph of the tool's `task.md`, followed by the rest of that file. It is Phase 1's final-round prompt adapted to a session that has the compiler.

## The tasks

The three tools in `bench/tools/`, frozen in #126 (MO-61). Hashes of each `SHA256SUMS` file, which hash the task text and every hidden file:

| Tool | `task.md` SHA-256 | `SHA256SUMS` SHA-256 |
| --- | --- | --- |
| linkcheck | `606c8ca694e2677ddaf54c089c4391f8decce073cf1f47270abe9c4b60cd3262` | `35a23403e748dc11bcbb9bfae5715626272224b33a2285962551dcb49f04df7c` |
| notioncopy | `9c3acdab662e00add439f824a88d9290a978d64a6722af8285acb08f34b2bf3a` | `75c360dbd8d20c63c62138599ae5d5edbeb0f6412db7ecb184fd8c0fbf073585` |
| tally | `77131346363b81c12c2ef03b8c9f212f2e06720ee01e2aeab83e5e01692a3ba0` | `261ef6933fc092dd0ed11bdfe0a49a7c2f07966060520a09d24b94b58716f46b` |

Before freezing, a coordinator's helper wrote each tool in Mo on the toolchain of the day and ran the hidden tests: tally 19 of 19 and notioncopy 22 of 22 on 7326ff5; linkcheck 20 of 21, its test 05 needing `Unicode16.lowercase`, which #170 added before this freeze.

## The sessions

One session per tool per family, six in all (D229), each a fresh Amp thread with no project, so no repository checkout and no project instructions:

- Anthropic: Claude Code with its default model, through Amp's Claude Code agent mode.
- OpenAI: Amp's GPT-6.1 Sol mode (`gpt61s`).

Release dates of the two models are not recorded here; they matter for held-out tasks, and this round has none.

## Grading

- Mechanical, protocol section 3: the grammar, `mo check`, then `python3 bench/tools/grade.py TOOL PROGRAM --mo` with the binary above.
- Model graders, protocol section 4: one Claude grader (Claude Code, default model) and GPT-6.1 Sol, each in a fresh session, blind, with `grader-prompt.md`.
- Human check seed: 6301. This round has 6 programs, fewer than 30, so all are graded and later rounds add to them.

## After the sessions

Each session's log is collected with `session.py collect`, tallied with `python3 learnability/retry.py`, and the programs, logs, grades, `notes.md` and `results.md` go in this folder.

Hashes of the frozen prompts:

    9779dabf77646bbae86cefa64a36afd0f7973f8065c741a86b7cc52d3b8e50c9  learnability/tools-diagnostic/prompt.md
    4d8cc5cf33edc8ce404f1a6bcf77dce1aa6971a46fbacdafe48ada865cb6b83a  learnability/tools-diagnostic/grader-prompt.md

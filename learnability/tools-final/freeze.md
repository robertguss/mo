# The three tools' final round: freeze

Frozen 24 Oct 2026, before any session saw a task (MO-63, protocol section 1). This is the final round D229 measures: at least 5 of the 6 attempts must pass. It follows the diagnostic round (`learnability/tools-diagnostic/`), whose findings were fixed first: MO-113 (#176), MO-114 (#180), MO-115 (#179), and linkcheck's hidden tests 21 and 22 (D275).

## The toolchain

- `mo` commit: 522c4c8a5874d26eedd5c2a163148b68e9f5e0e2 (main after #180).
- Built with `make CC=gcc build/gcc/debug/mo` (gcc 12.2.0, Debian 12) from a clean clone at `/home/user/.mo-frozen`, so the binary's built-in `lib/` path is `/home/user/.mo-frozen/lib`, the same path in every session orb.
- Binary SHA-256: `31d72f4370389c5a4bb90faae1bd70512f00b9e2f475a75c0b0ee52ec66ad301`. The harness refuses any other binary, and grading uses this one.
- Reference (`docs/reference/README.md`) SHA-256: `b910a44ae1c281d6218729d8301dd17eff0be7aa5a96f306b6efe59ad66ccebf`.
- Guide (`docs/agent-guide.md`) SHA-256: `9ba6a58edfcd8946bd344d68a03e2f15f09438bef7e34ebcc87a96fea8cf00a1`.
- Sessions read both only through `mo guide` and `mo guide --reference`, which print the copies embedded in the binary.
- Session package `mo-final-session.tgz`, SHA-256 `fc5f2f37ebba83729de955d7c1f64a124e28256e9c6db5f64d6b9c5a21e9bbc1`: the binary as `mo`, the commit's `lib/`, `learnability/harness/session.py`, and `setup.sh` from this folder with the binary's hash filled in. `setup.sh` runs `session.py start` with work folder `/home/user/workspace/mo-work` and session folder `/home/user/workspace/mo-session`, inside the workspace so Amp's file download can collect it (diagnostic finding 5), and puts the recording `mo` on `PATH`. Tested in a fresh orb on 24 Oct before freezing.

## The prompt

`prompt.md`, word for word, unchanged from the diagnostic round. `PACKAGE_URL` is the package's temporary download link, `NAME` the tool's name, and `TASK` the first paragraph of the tool's `task.md`, followed by the rest of that file.

## The tasks

The three tools in `bench/tools/`. tally and notioncopy are unchanged since #126 (MO-61); linkcheck has tests 21 and 22 added (D275). Hashes of each `SHA256SUMS` file, which hash the task text and every hidden file:

| Tool | `task.md` SHA-256 | `SHA256SUMS` SHA-256 |
| --- | --- | --- |
| linkcheck | `606c8ca694e2677ddaf54c089c4391f8decce073cf1f47270abe9c4b60cd3262` | `6b3e1095fb3deef266d804d805caa6627802ea043bc9ca87ae97c3f36c4f99b2` |
| notioncopy | `9c3acdab662e00add439f824a88d9290a978d64a6722af8285acb08f34b2bf3a` | `75c360dbd8d20c63c62138599ae5d5edbeb0f6412db7ecb184fd8c0fbf073585` |
| tally | `77131346363b81c12c2ef03b8c9f212f2e06720ee01e2aeab83e5e01692a3ba0` | `261ef6933fc092dd0ed11bdfe0a49a7c2f07966060520a09d24b94b58716f46b` |

## The sessions

One session per tool per family, six in all (D229), each a fresh Amp thread with no project, so no repository checkout and no project instructions:

- Anthropic: Claude Code with its default model, through Amp's Claude Code agent mode.
- OpenAI: Amp's GPT-6.1 Sol mode (`gpt61s`).

Release dates of the two models are not recorded here; they matter for held-out tasks, and this round has none.

## Grading

- Mechanical, protocol section 3: the grammar, `mo check`, then `python3 bench/tools/grade.py TOOL PROGRAM --mo` with the binary above.
- Model graders, protocol section 4: one Claude grader (Claude Code, default model) and GPT-6.1 Sol, each in a fresh session, blind, with `grader-prompt.md`, unchanged from the diagnostic round. A disagreement goes to a third fresh grader (Claude Opus 5.5). If that still leaves the graders and the tests in conflict, the coordinator decides it by running the program on the inputs in dispute, as Robert asked (D275), and records it as a finding.
- Human check, protocol section 5: Robert does not grade programs himself ("I dont want to do a blind human review and trust your judgement", D275), so as in the diagnostic round the model graders' answers raise findings only. The hidden tests decide the verdict.

## After the sessions

Each session's `mo-session` folder is downloaded, checked with `session.py check-log`, tallied with `python3 learnability/retry.py`, and the programs, logs, grades, `notes.md` and `results.md` go in this folder.

Hashes of the frozen prompts:

    9779dabf77646bbae86cefa64a36afd0f7973f8065c741a86b7cc52d3b8e50c9  learnability/tools-final/prompt.md
    4d8cc5cf33edc8ce404f1a6bcf77dce1aa6971a46fbacdafe48ada865cb6b83a  learnability/tools-final/grader-prompt.md

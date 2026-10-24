# Benchmark v0, round 2: freeze

Frozen 24 Oct 2026, before any session saw a task (MO-138, protocol section 1). This round measures Phase 2's first-retry targets again, after MO-137 (#201) changed the E0051, E0075 and E0050 diagnostics that round 1's slow fixes came from. It follows round 1's method (`../bench-v0-round-1/notes.md`) with the same tasks, a binary frozen from main as it was at the freeze, the same two families, and the same blind graders. Only two things differ from round 1: the binary, and 02-col-sum's two new hidden runs.

## The toolchain

- `mo` commit: c7ce9c96fb3d89d9c9578ae5f8ee7181c658dc45 (main after #201).
- Built with `make CC=gcc build/gcc/debug/mo build/gcc/debug/mo-grammar` (gcc 12.2.0, Debian 12) from a clean clone at `/home/user/.mo-frozen`, so the binary's built-in `lib/` path is `/home/user/.mo-frozen/lib`, the same path in every session orb.
- Binary SHA-256: `6f52dbbff8208dba363da8d9984300e7d0bdfa96fc911902dadbc60e442cb3c1`. The harness refuses any other binary, and grading uses this one.
- Reference (`docs/reference/README.md`) SHA-256: `b1b2a913af0d04bba2ef3f3440e9ed80f9bcaaa5c2a5cd1220a9dd40f6a55ced`.
- Guide (`docs/agent-guide.md`) SHA-256: `d813f427f567b94feb0322915eead0bd6e17d4936530dd0df021b27b57187c50`.
- Sessions read both only through `mo guide` and `mo guide --reference`.
- `python3 bench/v0/grade.py --check` with this binary: 20 of 20 tasks frozen and sound.
- `learnability/harness/session.py` is unchanged from c7ce9c9.

## The packages

As round 1's: one per task, holding the binary as `mo`, the commit's `lib/`, `learnability/harness/session.py`, `setup.sh` from this folder with the binary's hash filled in, and the task's `start/` folder as `start/` (empty for the five write-a-program tasks). `prompt.md` and `setup.sh` are round 1's, byte for byte. The 06-temperature package was tested in a fresh orb with no project before freezing: its hash matched, setup printed `ready`, the recording `mo` was on `PATH`, the work folder held the two start files, `mo check temperature.mo` reported the planted errors (E0050, E0070, E0075 and E0051), and `session.py check-log` accepted the log.

## The tasks

The twenty tasks in `bench/v0/`, unchanged from round 1 except 02-col-sum. That task's text is unchanged; two hidden runs were added before any round 2 session started, because round 1 found its tests had no blank first line (round 1 `results.md`, finding 1):

- `14-blank-first-line`: input `\n1\n` prints `: 1.00`, status 0.
- `15-blank-only`: input `\n` prints `: 0.00`, status 0.

The reference passes both. Both of round 1's 02-col-sum programs fail `15-blank-only`.

Tasks 05, 10, 15 and 20 were held out in round 1. Round 1's sessions and graders have since seen them, so they are no longer held out (`bench/v0/README.md`, "Held-out tasks"). This round reports no held-out result.

| Task | Kind | `SHA256SUMS` SHA-256 | Package SHA-256 |
| --- | --- | --- | --- |
| 01-rle | write | `5a37715561a6283bf53922eb7cd419117c0d0d4b1375a0f6ce450e38ae5f8886` | `fb070b9301c0a4a3cfd8c3bd4e0c5a8dfd78f98c1b117e693d7ad341c9284372` |
| 02-col-sum | write | `6d0af9a39a5ebb72926d99dd342d2bb47bc0d1eb08a8c21a1c29dba52d96da1c` | `34ddd89016b32b9c10edfe51f800455eb49df229ed45deb12279d60463449ba3` |
| 03-brackets | write | `2b26c56cdce6ac0f962e3a591bcd959ad59f8b1db0d1398c55a36fd447e6cb00` | `34ddd89016b32b9c10edfe51f800455eb49df229ed45deb12279d60463449ba3` |
| 04-line-counts | write | `9b65501c989efe370cca8a5aebf40fd6fa7b31954bb5cf5ba796d1a5f0b2741a` | `34ddd89016b32b9c10edfe51f800455eb49df229ed45deb12279d60463449ba3` |
| 05-ranges | write | `e3c81208b020a771b07b496ac409a21594aabcb149a44a87ac15948ec9078a95` | `2e130bb5fa365b0804366bc49108a3e4528c020e0ecfacfb7c25811bf474e1f6` |
| 06-temperature | fix | `6dd504fe3e0eba5ddab0996918cb3b6bd856f7dc02972a8b65f099da20ae342e` | `6873ad42acbfceaa04eeef02c35d622872179fb2f739e8c6d48cb33df685e799` |
| 07-grades | fix | `a41759bb4db8889ae5707671d85c5978863aa0b401aabaa8e0bff4478386b951` | `72fc7fbefa881f1773f4ee449fca5855c1f8874f0fe76246795addb2f7538c6d` |
| 08-playlist | fix | `5ea4c43777e761257432bf29352fbf98b65e524da9a47f1e946f96c6addd8211` | `f0cdfa00055d0e7459f1cc254b355a1621b6d840ffca719ab0f7021bbe0bae5d` |
| 09-rpn | fix | `a1a3c0c2d4c90a12a36ba4f7a6510675eac7794242d12c773d16be7353e0dca0` | `fb46240f973739ad842e7e2e3ff431901d151aaa42605e48f4daca1fd6beb5cd` |
| 10-restock | fix | `e751acac69fa118f2263af0eee487cb7ce9772c04befcdcebd6227ae5c16dac6` | `4b68e25c84bc8a9c1fe7ca3714ee716b9cd6c27df5e1503781ee0ea6e49e8bea` |
| 11-calendar | failing-test | `368f33e05ec9ac6f49efcfdfd8ae0b7f1ea6da08246e08ae32cc5490bfce9132` | `d07ca95de965e9049ab6646eaa8d3f5756c6a073a132c4ba0a165ad4dc81ab23` |
| 12-search | failing-test | `bd0392ac299d6de38b25bee789f69b30b6719751185d4d24691788edbe0c9610` | `f8c1a6f4dc5e64f521026076ee52708c3fb61b4b536fb8dfe5b898eed5533d49` |
| 13-titles | failing-test | `9d2f9b6da102b9c8a62685e6cf2ab0cd9e1ee2b7a4401b97ebdc0b57ca16b1c0` | `ff254b0a616c56143ca4b970f9a3caee499824678bb65508c1350f50924104d8` |
| 14-wallet | failing-test | `8e053e9c9feeee4b5f3b6bb05649bde10739f54763b33845ca239a0b0469b2c0` | `d04fa55997c583add0ef7b21f577eb45bf436492168fbd9e4e03f377cf4a7cff` |
| 15-parking | failing-test | `6e552a670929f3890fb2a4af1ea11a48a080a65de81b4f75419746b52ba9bedc` | `bb4ec1bf5277b395078365ca34880805745f0d98245d8ab37f9aeecd4d4edcdb` |
| 16-todo | extend | `1ff5b92e3c6f1e4810baa6cfb98f02a084471005979b011f640fc518c06a4ff8` | `b7eca9eb437327680f15cfbbdbec780dfe5abc756d167e84015831aa0cbb919e` |
| 17-stats | extend | `71bee114eba88839793df10ec0a5963d786cbb8ebc2785e101752d0dbc88e68a` | `a2e60389e278f41d4afdd642e517135c247e618919f452ed456abb97b051b766` |
| 18-store | extend | `a988acd18005e1139f935c8a2a1d3a21e520d7c24a1f88680c6598956714e7a2` | `9b157d4afd1faf1dc06e591ea6fa136a27e781854b080c664816857bab72c11a` |
| 19-cart | extend | `cb5eeac51ceddeb03c280244d012bb7341881431de347a5d1b494b616724d6fe` | `c19d44e8fa3daea6155fd1f9c7c9500b680119908372f48eaec77d906365a66e` |
| 20-timesheet | extend | `c06386118e3001702258fdf5c9138d186cab65c65f7dc94b7ecc251eef1fd387` | `b1742d2c839d9699d4d4dd25bac7d425530d4d39ff8eb7e05daaacd1f31b95eb` |

## The sessions

One session per task per family, forty in all (D255), each a fresh Amp thread with no project, so no repository checkout and no project instructions:

- Anthropic: Claude Code with its default model, through Amp's Claude Code agent mode (`external-agent:claude-code`, no features).
- OpenAI: Amp's GPT-6.1 Sol mode (`gpt61s`).

## Grading

As round 1's:

- Mechanical, protocol section 3: `python3 bench/v0/grade.py TASK SESSION/work --mo MO --grammar MO_GRAMMAR` with the binary and grammar above.
- Model graders, protocol section 4: grader A in Claude Code (default model) and grader B in GPT-6.1 Sol, each in a fresh session, blind, with `grader-prompt.md`, unchanged. A disagreement goes to grader C in a fresh Claude Code session with Claude Opus 5.5 at high effort. A conflict that remains is decided by running the program on the inputs in dispute (D275) and recorded as a finding. Identifiers are drawn with seed `v0r2-ids` and the orders with `v0r2-A` and `v0r2-B`.
- Human check, protocol section 5: Robert does not grade programs himself (D275).

## The target

Phase 2's first-retry item: at least 85% of compile errors fixed on the next attempt, counted by `python3 learnability/retry.py` without E1103, E1150 and E1400, as in round 1. The item is met only if the point estimate reaches 85%.

Hashes of the frozen prompts, the same as round 1's:

    fa333ef5811cb49f4d3524da0bfc44a49f8877f38cad4c3425da6d615bb332e2  learnability/bench-v0-round-2/prompt.md
    4d8cc5cf33edc8ce404f1a6bcf77dce1aa6971a46fbacdafe48ada865cb6b83a  learnability/bench-v0-round-2/grader-prompt.md

# Benchmark v0, round 3: freeze

Frozen 24 Oct 2026, before any session saw a task (MO-148, protocol section 1). This round measures Phase 2's first-retry targets again, after MO-147 (#212) added a rule to the guide's loop: fix every error `mo check` reports before running it again (D296). It follows round 2's method (`../bench-v0-round-2/notes.md`) with the same tasks, a binary frozen from main as it was at the freeze, the same two families, and the same blind graders. Only two things differ from round 2: the binary, and 11-calendar's new hidden test.

## The toolchain

- `mo` commit: 31c0f8c70d09d2b63b6a046ee4187c830ea19e24 (main after #212).
- Built with `make CC=gcc build/gcc/debug/mo build/gcc/debug/mo-grammar` (gcc 12.2.0, Debian 12) from a clean clone at `/home/user/.mo-frozen`, so the binary's built-in `lib/` path is `/home/user/.mo-frozen/lib`, the same path in every session orb.
- Binary SHA-256: `52dffb361aad8f2809f6bc7c534e066c3af1bc88cef0872b6b021985d3385466`. The harness refuses any other binary, and grading uses this one.
- Reference (`docs/reference/README.md`) SHA-256: `abee03274ba5487db656889b346c6262b481e30cf68e99245e96208ab0877a5d`.
- Guide (`docs/agent-guide.md`) SHA-256: `06d16fa5c7d88fb9e0f5231b310b56d0dda20684a67b9c8be28a0005cb52611a`. Its loop's step 3 reads: "Run `mo check FILE`. It reports each error it finds, in source order. Fix every error it reports before you run `mo check` again, not only the first."
- Sessions read both only through `mo guide` and `mo guide --reference`.
- `python3 bench/v0/grade.py --check` with this binary: 20 of 20 tasks frozen and sound.
- `learnability/harness/session.py` is unchanged from c7ce9c9, round 2's commit.

## The packages

As round 2's: one per task, holding the binary as `mo`, the commit's `lib/`, `learnability/harness/session.py`, `setup.sh` from this folder with the binary's hash filled in, and the task's `start/` folder as `start/` (empty for the five write-a-program tasks). `prompt.md`, `setup.sh` and `grader-prompt.md` are round 1's, byte for byte. The 06-temperature package was tested in a fresh orb with no project before freezing: its hash matched, setup printed `ready`, the recording `mo` was on `PATH`, the work folder held the two start files, `mo check temperature.mo` reported the planted errors (E0050, E0070, E0095 and E0051; MO-141, #207, reports the old E0075 there as E0095), `mo guide` printed the new step 3, and `session.py check-log` accepted the log.

## The tasks

The twenty tasks in `bench/v0/`, unchanged from round 2 except 11-calendar. That task's text is unchanged; one hidden test was added before any round 3 session started, because round 2 found that a closed-form `days_between` which overflows at the smallest `Int` passed every hidden test (round 2 `results.md`, finding 2):

- `HiddenTests` test 4, "a leap year at the smallest Int": `Calendar.days_between(-9223372036854775808, -9223372036854775807) == 366`.

The reference passes it. Round 2's Claude Code program stops on it with an `Int` overflow; round 1's two programs and round 2's GPT-6.1 Sol program pass it.

No task is held out (round 2 `freeze.md`).

| Task | Kind | `SHA256SUMS` SHA-256 | Package SHA-256 |
| --- | --- | --- | --- |
| 01-rle | write | `5a37715561a6283bf53922eb7cd419117c0d0d4b1375a0f6ce450e38ae5f8886` | `22b3ee1f08c5b3b1b1a59be65b30086df18bca618692c4a6795ebf8e10f26957` |
| 02-col-sum | write | `6d0af9a39a5ebb72926d99dd342d2bb47bc0d1eb08a8c21a1c29dba52d96da1c` | `22b3ee1f08c5b3b1b1a59be65b30086df18bca618692c4a6795ebf8e10f26957` |
| 03-brackets | write | `2b26c56cdce6ac0f962e3a591bcd959ad59f8b1db0d1398c55a36fd447e6cb00` | `22b3ee1f08c5b3b1b1a59be65b30086df18bca618692c4a6795ebf8e10f26957` |
| 04-line-counts | write | `9b65501c989efe370cca8a5aebf40fd6fa7b31954bb5cf5ba796d1a5f0b2741a` | `6b6dabf9e184a46fd1e2ddb4a6b6322f1bbc68d495b83d239d2671d1206fc716` |
| 05-ranges | write | `e3c81208b020a771b07b496ac409a21594aabcb149a44a87ac15948ec9078a95` | `6b6dabf9e184a46fd1e2ddb4a6b6322f1bbc68d495b83d239d2671d1206fc716` |
| 06-temperature | fix | `6dd504fe3e0eba5ddab0996918cb3b6bd856f7dc02972a8b65f099da20ae342e` | `98e00d263171ba5f3d20da8ec89ca8b1c7493867f3da68d99faa743e8487c5cd` |
| 07-grades | fix | `a41759bb4db8889ae5707671d85c5978863aa0b401aabaa8e0bff4478386b951` | `39baa31016635a1dfec79566cf69f3dbfbbf46b5db78dc64cf2afcfa700ef9c5` |
| 08-playlist | fix | `5ea4c43777e761257432bf29352fbf98b65e524da9a47f1e946f96c6addd8211` | `0ae40ba8efba7d9ec392c24b09db8ffe409324778a33dfecef6a18ec02aabd8d` |
| 09-rpn | fix | `a1a3c0c2d4c90a12a36ba4f7a6510675eac7794242d12c773d16be7353e0dca0` | `d82b247515cbc462a842b20be12bddf44f514ae45a6eca04d1eae52878d799bd` |
| 10-restock | fix | `e751acac69fa118f2263af0eee487cb7ce9772c04befcdcebd6227ae5c16dac6` | `003f215f6c1273ba935a346071ece1282d9c5a36cf12fd6e1d34d2fddc6771e5` |
| 11-calendar | failing-test | `4a7ba31b027b51b7efc6380b23c07885e7a7d6548bd7aa6105a1f9b10f95def2` | `9203afedee842133f21528ce6e8b3df812ff309a0765922592b73dc98e2be0f3` |
| 12-search | failing-test | `bd0392ac299d6de38b25bee789f69b30b6719751185d4d24691788edbe0c9610` | `74744d75faebd6926839732f6e7dfe0d6d40920c2f01365af2d3d91712f7d519` |
| 13-titles | failing-test | `9d2f9b6da102b9c8a62685e6cf2ab0cd9e1ee2b7a4401b97ebdc0b57ca16b1c0` | `5dadfd34ac40734aab355b1e0087fb87313f33a7afdd955e813af61331edc2db` |
| 14-wallet | failing-test | `8e053e9c9feeee4b5f3b6bb05649bde10739f54763b33845ca239a0b0469b2c0` | `96b4f0cd6c1dcb403d20656763f2bc8d7d867e077a3ab3f23187313ba75ead87` |
| 15-parking | failing-test | `6e552a670929f3890fb2a4af1ea11a48a080a65de81b4f75419746b52ba9bedc` | `d8ae2075d94895624d55e4f44a5ddb127b710a2a62c0165c31439a90031315c2` |
| 16-todo | extend | `1ff5b92e3c6f1e4810baa6cfb98f02a084471005979b011f640fc518c06a4ff8` | `cd4ba63ba3ac6b24ef03c992781c1f92287a53fdd35ab0f6797186758ae1079c` |
| 17-stats | extend | `71bee114eba88839793df10ec0a5963d786cbb8ebc2785e101752d0dbc88e68a` | `d9ccf8a48fd371c3a956a4e4281018ad6ee87396879eb7330918e87a88ba5a4a` |
| 18-store | extend | `a988acd18005e1139f935c8a2a1d3a21e520d7c24a1f88680c6598956714e7a2` | `2f21637c06064221470af014d4d5b659c7337e0f45d56f81abb0841839a1d6a1` |
| 19-cart | extend | `cb5eeac51ceddeb03c280244d012bb7341881431de347a5d1b494b616724d6fe` | `fec23aff0fc8a18da7917d20fa4b09efd1d7718893236ad48941953d2586f52b` |
| 20-timesheet | extend | `c06386118e3001702258fdf5c9138d186cab65c65f7dc94b7ecc251eef1fd387` | `303aaba126de8d69a41c28aad449c130b664e395cf3625497be09f56e121737e` |

## The sessions

One session per task per family, forty in all (D255), each a fresh Amp thread with no project, so no repository checkout and no project instructions:

- Anthropic: Claude Code with its default model, through Amp's Claude Code agent mode (`external-agent:claude-code`, no features).
- OpenAI: Amp's GPT-6.1 Sol mode (`gpt61s`).

## Grading

As round 2's:

- Mechanical, protocol section 3: `python3 bench/v0/grade.py TASK SESSION/work --mo MO --grammar MO_GRAMMAR` with the binary and grammar above.
- Model graders, protocol section 4: grader A in Claude Code (default model) and grader B in GPT-6.1 Sol, each in a fresh session, blind, with `grader-prompt.md`, unchanged. A disagreement goes to grader C in a fresh Claude Code session with Claude Opus 5.5 at high effort. A conflict that remains is decided by running the program on the inputs in dispute (D275) and recorded as a finding. Identifiers are drawn with seed `v0r3-ids` and the orders with `v0r3-A` and `v0r3-B`.
- Human check, protocol section 5: Robert does not grade programs himself (D275).

## The target

Phase 2's first-retry item: at least 85% of compile errors fixed on the next attempt, counted by `python3 learnability/retry.py` without E1103, E1150 and E1400, as in rounds 1 and 2. The tally is unchanged (D296). The item is met only if the point estimate reaches 85%. The share of errors a session edited before its next run is reported beside it as a secondary number, not as the item's measure.

Hashes of the frozen prompts, the same as rounds 1 and 2:

    fa333ef5811cb49f4d3524da0bfc44a49f8877f38cad4c3425da6d615bb332e2  learnability/bench-v0-round-3/prompt.md
    4d8cc5cf33edc8ce404f1a6bcf77dce1aa6971a46fbacdafe48ada865cb6b83a  learnability/bench-v0-round-3/grader-prompt.md

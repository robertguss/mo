# Benchmark v0, round 1: freeze

Frozen 24 Oct 2026, before any session saw a task (MO-62, protocol section 1). Robert said to start it on 24 Oct ("Start it now"). This round is Phase 2's baseline, which later phases are compared with; it has no pass target of its own (protocol section 7). Its sessions' errors also count towards the first-retry targets.

## The toolchain

- `mo` commit: 267370f50a13e0ce12d103346ae63b6861ba3436 (main after #178).
- Built with `make CC=gcc build/gcc/debug/mo build/gcc/debug/mo-grammar` (gcc 12.2.0, Debian 12) from a clean clone at `/home/user/.mo-frozen`, so the binary's built-in `lib/` path is `/home/user/.mo-frozen/lib`, the same path in every session orb.
- Binary SHA-256: `c391c58d96bfb36104a0525dbebf7988ce36496f512a26318c38620ae72cc587`. The harness refuses any other binary, and grading uses this one.
- Reference (`docs/reference/README.md`) SHA-256: `b910a44ae1c281d6218729d8301dd17eff0be7aa5a96f306b6efe59ad66ccebf`.
- Guide (`docs/agent-guide.md`) SHA-256: `9ba6a58edfcd8946bd344d68a03e2f15f09438bef7e34ebcc87a96fea8cf00a1`, the same as the tools' final round.
- Sessions read both only through `mo guide` and `mo guide --reference`.
- `python3 bench/v0/grade.py --check` with this binary: 20 of 20 tasks frozen and sound.

## The packages

The 06-temperature package was tested in a fresh orb with no project before freezing: its hash matched, setup printed `ready`, the recording `mo` was on `PATH`, the work folder held the two start files, `mo check temperature.mo` reported the planted errors, and `session.py check-log` accepted the log, whose runs copied both files.

One package per task: the binary as `mo`, the commit's `lib/`, `learnability/harness/session.py`, `setup.sh` from this folder with the binary's hash filled in, and the task's `start/` folder as `start/` (empty for the five write-a-program tasks). `setup.sh` starts the harness with work folder `/home/user/workspace/mo-work` and session folder `/home/user/workspace/mo-session`, copies `start/` into the work folder, and puts the recording `mo` on `PATH`. The session never sees `hidden/`.

## The prompt

`prompt.md`, word for word. It is the tools' prompt with step 3 and two short wordings changed, because a task here names files to write or change in a folder that may already hold some. `PACKAGE_URL` is the task's package link and `TASK` is the task's whole `task.md`.

## The tasks

The twenty tasks in `bench/v0/` (#149, MO-62), unchanged since. Tasks 05, 10, 15 and 20 are held out: written 22 Oct 2026, seen by no session, grader or published document. Protocol section 6 asks that held-out tasks postdate every model taking part; the two models' release dates are not recorded here, so the held-out results are reported with that caveat.

| Task | Kind | Held out | `SHA256SUMS` SHA-256 | Package SHA-256 |
| --- | --- | --- | --- | --- |
| 01-rle | write |  | `5a37715561a6283bf53922eb7cd419117c0d0d4b1375a0f6ce450e38ae5f8886` | `3deb13827276d900b3ceacec3f050246a54c8a7068afb237ea6de00f1423efb3` |
| 02-col-sum | write |  | `94281f9ec05745439569299809688c5ad9f8487f2d34f84ea6be0ccafc990819` | `3deb13827276d900b3ceacec3f050246a54c8a7068afb237ea6de00f1423efb3` |
| 03-brackets | write |  | `2b26c56cdce6ac0f962e3a591bcd959ad59f8b1db0d1398c55a36fd447e6cb00` | `3deb13827276d900b3ceacec3f050246a54c8a7068afb237ea6de00f1423efb3` |
| 04-line-counts | write |  | `9b65501c989efe370cca8a5aebf40fd6fa7b31954bb5cf5ba796d1a5f0b2741a` | `3deb13827276d900b3ceacec3f050246a54c8a7068afb237ea6de00f1423efb3` |
| 05-ranges | write | yes | `e3c81208b020a771b07b496ac409a21594aabcb149a44a87ac15948ec9078a95` | `4bfe7e2e2bd0df0899075c600932bd745141f25eb692184a68765c023b0aa0b7` |
| 06-temperature | fix |  | `6dd504fe3e0eba5ddab0996918cb3b6bd856f7dc02972a8b65f099da20ae342e` | `773a2db0fb113eee3992a64fdd84f3325f6fa49ddd55a0d8447c7641001201d6` |
| 07-grades | fix |  | `a41759bb4db8889ae5707671d85c5978863aa0b401aabaa8e0bff4478386b951` | `072c99804c0baa49231a6c9b831f1daccbe695851105fcad152e7827410b37ae` |
| 08-playlist | fix |  | `5ea4c43777e761257432bf29352fbf98b65e524da9a47f1e946f96c6addd8211` | `e53220601d0cfa5a1a1d91e32a95e8009fb5e52f7830bad6fae85eacf2e3ea55` |
| 09-rpn | fix |  | `a1a3c0c2d4c90a12a36ba4f7a6510675eac7794242d12c773d16be7353e0dca0` | `3dc8d457298be677123fff985fb62af3264a0366b32e312eb1d928f2fa97db8f` |
| 10-restock | fix | yes | `e751acac69fa118f2263af0eee487cb7ce9772c04befcdcebd6227ae5c16dac6` | `cde7100fc116c7ce477ddf2c6c0cc953a0e302515d3635ba1bd95bb402aaac74` |
| 11-calendar | failing-test |  | `368f33e05ec9ac6f49efcfdfd8ae0b7f1ea6da08246e08ae32cc5490bfce9132` | `55e000a5c5405f92ddc97bb9c0f8d25be06ca7ee22dd5e6d26e1db3763cfd041` |
| 12-search | failing-test |  | `bd0392ac299d6de38b25bee789f69b30b6719751185d4d24691788edbe0c9610` | `e41c6068234424103143466ea8ab83616e39e4ed2f783b3c90a8f0b0844117e4` |
| 13-titles | failing-test |  | `9d2f9b6da102b9c8a62685e6cf2ab0cd9e1ee2b7a4401b97ebdc0b57ca16b1c0` | `ef7e0914689dbc6c1985dd7c2f3615f89c77b78150879509892d86bed0e96c17` |
| 14-wallet | failing-test |  | `8e053e9c9feeee4b5f3b6bb05649bde10739f54763b33845ca239a0b0469b2c0` | `d12d31abf5c073f7b0c6ad797be837e78aca0eaeb9112973debc94a174891e64` |
| 15-parking | failing-test | yes | `6e552a670929f3890fb2a4af1ea11a48a080a65de81b4f75419746b52ba9bedc` | `e27b2c82437741e76c4702698952a1e0c482b79af4ea45a9787d16fa24e587c4` |
| 16-todo | extend |  | `1ff5b92e3c6f1e4810baa6cfb98f02a084471005979b011f640fc518c06a4ff8` | `e48df61e2df821dfd0eee164a607dc778e729a63d1c86d261d7270bdaabcb957` |
| 17-stats | extend |  | `71bee114eba88839793df10ec0a5963d786cbb8ebc2785e101752d0dbc88e68a` | `24cc1ba408876aa93864bdc5d82268bce707c27b7007c2623dfe3b4e58b1cf93` |
| 18-store | extend |  | `a988acd18005e1139f935c8a2a1d3a21e520d7c24a1f88680c6598956714e7a2` | `e2159505d75ca757c5932ba0eff0a9cf506337281a20b53cec572caceea62952` |
| 19-cart | extend |  | `cb5eeac51ceddeb03c280244d012bb7341881431de347a5d1b494b616724d6fe` | `059a4e894a4a5ccdb200fda76b01bc54dddc4a86c07c5f5fdd6e77ac4cf0a4e6` |
| 20-timesheet | extend | yes | `c06386118e3001702258fdf5c9138d186cab65c65f7dc94b7ecc251eef1fd387` | `bd6209fe727bcd5a911264eeb216d3d9f8b05a641ac3f228135b107cbb201e79` |

## The sessions

One session per task per family, forty in all (D255), each a fresh Amp thread with no project, so no repository checkout and no project instructions:

- Anthropic: Claude Code with its default model, through Amp's Claude Code agent mode.
- OpenAI: Amp's GPT-6.1 Sol mode (`gpt61s`).

## Grading

- Mechanical, protocol section 3: `python3 bench/v0/grade.py TASK SESSION/work --mo MO --grammar MO_GRAMMAR` with the binary and grammar above. A folder whose only diagnostics are E1103 or E1150 is a toolchain gap only when each is on a form the reference requires (`bench/v0/README.md`).
- Model graders, protocol section 4: Claude Code (default model) and GPT-6.1 Sol, each in a fresh session, blind, with `grader-prompt.md`, unchanged from the tools' rounds. A disagreement goes to a third fresh grader (Claude Opus 5.5). A conflict that remains is decided by running the program on the inputs in dispute (D275) and recorded as a finding.
- Human check, protocol section 5: Robert does not grade programs himself (D275), so the model graders' answers raise findings only and the hidden tests decide each verdict.

## After the sessions

Each session's work folder and `mo-session` folder are collected, the logs checked with `session.py check-log` and tallied with `python3 learnability/retry.py`, and the folders, logs, grades, `notes.md` and `results.md` go in this folder.

Hashes of the frozen prompts:

    fa333ef5811cb49f4d3524da0bfc44a49f8877f38cad4c3425da6d615bb332e2  learnability/bench-v0-round-1/prompt.md
    4d8cc5cf33edc8ce404f1a6bcf77dce1aa6971a46fbacdafe48ada865cb6b83a  learnability/bench-v0-round-1/grader-prompt.md

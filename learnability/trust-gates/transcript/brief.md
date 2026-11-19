You are working alone in this folder. It is your whole world for this task.

Rules:
- Read, list and run only what is inside this folder. Do not read, list or search any path
  outside it, do not use the network or web search, and do not look for other copies of these
  files or for the project they came from. This rule is the experiment's integrity; breaking it
  spoils the result.
- Work only in `work/`. `frozen/` is the original program, kept for comparison; do not change it.

What is here:
- `toolchain/mo`: the Mo toolchain. Run it as `./toolchain/mo`. Its standard library is in
  `toolchain/lib/`.
- `guide.txt`: what `./toolchain/mo guide` prints, the toolchain's guidance for agents. Read it
  first. `./toolchain/mo guide --reference` prints the language reference.
- `frozen/`: a task-queue service in Mo: `main.mo` (the service), `job-queue.mo` (its queue
  logic) and `queue-tests.mo` (its tests, which use both).
- `work/`: a copy of those three files for you to change.

Your task: make `./toolchain/mo test --gates work/queue-tests.mo` pass, with exit status 0.
That command runs every check (promises, examples, tests and the service's checks), then the
stated-intent rule, then the mutation gate, and reports what fails. Use only what it reports
and the guide to decide what to change.

How to change the program:
- Keep what the service does the same, and keep every declaration's name and signature.
- Resolve a missing statement of intent by writing a promise or an example, as the report says.
- Resolve a surviving mutant with a new check that kills it, or, when you judge no check could
  kill it, by simplifying the code so the mutant no longer exists. Never delete or weaken an
  existing check.
- While you work you may shorten runs with
  `./toolchain/mo test --gates work/queue-tests.mo --against frozen/queue-tests.mo`. Finish with
  the full run, with no option except `--json` if you want it.

Long runs: a full run can take several minutes and a single shell command here is stopped
after about ten. Start long runs in the background with their output in a file, for example
`setsid nohup ./toolchain/mo test --gates work/queue-tests.mo > run-1.txt 2>&1 < /dev/null &`,
then wait for them in loops that each last under nine minutes, for example
`timeout 530 sh -c 'while ! grep -q "passed, " run-1.txt; do sleep 15; done'; tail -5 run-1.txt`.
Keep every run's output file.

Keep a log in `log.md` as you go. For each surviving mutant the gate reported, write one line:
the mutant as reported, and how you resolved it (the new check, or the simplification). Mark
any you judged no check could kill, and any that only one particular sequence of calls to the
service could kill.

When you finish, reply with: the exit status and the last lines of the final full run, how many
checks you added by kind, how many code changes you made, and the two counts from your log
(judged unkillable, and killed only by one particular call sequence).

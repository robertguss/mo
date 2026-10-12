---
name: write-skills
description: Create or revise Mo agent skills with sharp triggers, progressive disclosure and pruned failure modes, then validate with the skills validator.
---

# Write Skills (Mo)

Adapted from dzhng/skills `write-skills` (MIT) for Mo's project skills in `.agents/skills/<name>/SKILL.md` (the canonical store; `.claude/skills/<name>` symlinks to it).

## Principles

1. **Trigger is the interface.** The frontmatter `description` must say what the skill does AND when to reach for it, in one sentence, so a cold agent routes correctly without opening the body. Test it: would this fire on the right request and stay silent on neighbours?
2. **Lead with principles, then workflow.** State the standard the skill holds itself to before steps; a judge (human or `eval-skills`) grades against those principles.
3. **Progressive disclosure.** Body holds what every run needs; bulky references go in sibling files linked from the body, loaded only when a step needs them. Keep the body skimmable mid-task.
4. **Ground in this repo.** Name real paths and commands that exist (`make formal`, `grammar/mo.y`, `tests/test.h`), never placeholders. A referenced command that doesn't exist is a bug in the skill — verify it.
5. **Prune failure modes.** Every rule should trace to a failure it prevents (see `correct`: encode lessons in structure where possible; prose is the weakest enforcement). Delete rules that only restate the obvious.
6. **One owner per idea.** Overlapping skills cause routing confusion: `write-tests` owns how to write a test, `audit-tests` owns whether it earns its cost, `correct` owns making mistakes impossible. Link, don't duplicate.

## Workflow

Draft the `SKILL.md` (frontmatter `name` exactly matching the directory), then run `muse skills validate .agents/skills/<name> --json` and fix every diagnostic. For behaviour changes, eval with `eval-skills` against at least one golden case before calling it done. Keep Mo's compressed principles in mind (test behaviour not implementation, prove it works, sequence verifiable units) as body content, not as separate principle skills.

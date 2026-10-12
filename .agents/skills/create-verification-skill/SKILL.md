---
name: create-verification-skill
description: Generate a project-local verification skill that drives the Mo toolchain the way a user does and captures evidence that proves behaviour.
---

# Create a Verification Skill (Mo)

Adapted from pstack `create-verification-skill` by poteto/Lauren Tan (MIT, via cursor/plugins) for Mo.

Every serious project needs a scripted way to drive the real artifact and prove behaviour. This skill generates that as a project-local skill (for Mo: `verify-mo`, already seeded in this repo) tailored to what the repo actually does. You write for the next agent, reading cold mid-task.

## 1. Interview the repo, not the user

Answer from the codebase; ask only what you cannot observe:

- **Surface:** what does a user touch? For Mo today: the `mo` CLI (`src/tools/`), the Core text form, and the executable grammar checker. Note the rest.
- **Run:** the repo's own commands — `make` builds `build/<compiler>/debug/mo`; choose `CC=gcc` or `CC=clang`. Note toolchain needs (C compiler, bison for grammar, elan/Lean for `make formal`).
- **Drive:** existing harnesses first (`tests/test_*.c`, `grammar/tests/`, `examples/*.mo`, `fuzz/corpus/`), then the built binary with exact invocations.
- **Observe:** exit codes, stdout/stderr, diagnostics (`src/base/diag.*`), files written. State what evidence proves each claim.
- **Isolate:** builds are per-compiler under `build/`; use a scratch dir for outputs. Never drive or kill a user's long-lived process.

If the checkout doesn't build, fix that first (or report it precisely) — a skill written against a broken base teaches wrong steps.

## 2. Generate the skill

Write `.agents/skills/verify-<app>/SKILL.md` with frontmatter (`name`, one-sentence `description`) and sections grounded in the interview, no placeholders: **Launch** (exact build command + readiness signal), **Doctor** (one read-only worth-driving check), **Drive** (real commands from this repo), **Evidence** (what to capture and where; exercise the real user path, verify side effects, not just exit 0), **Cleanup** (remove only what you started; evidence survives), **Helpers** (any script is executable with its invocation shown).

## 3. Seed the feature map

List the top 3-5 user-facing behaviours, one entry each: what it is, how to reach it, the driving command, the observable end state that proves it, and gotchas. The map is the maintained verification source; one convenient entry point is incomplete coverage.

## 4. Prove it before handing over

Run the generated skill end-to-end once: launch, doctor, drive ONE mapped feature, capture evidence, clean up, confirm evidence still exists. Fix what fails; run cleanup after every failed attempt. A generated skill never executed is a draft. If you proved it in a stand-in (missing Lean/bison locally), list what remains unverified on a full toolchain.

Point the user at keeping the map honest as the CLI grows (a `maintain-verification-skill` pass: source wave + one live pass, at most one batch of proven corrections).

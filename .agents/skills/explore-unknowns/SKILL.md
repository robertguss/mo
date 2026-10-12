---
name: explore-unknowns
description: Map a Mo task unknowns quadrant by quadrant before planning, ending with a complete four-quadrant map that feeds write-spec.
---

# Explore Unknowns (Mo)

Adapted from dzhng/skills `explore-unknowns` (MIT) for Mo. Use when a request is ambiguous, the subsystem is unfamiliar, or the user will "know it when they see it." The map is the deliverable; implementation starts only after handover to `write-spec`.

## The quadrant walk (one stage at a time)

1. **Known knowns** — scan the territory first: relevant `src/` modules, `docs/design/`, `formal/`, tests and grammar. Open with the settled ground, cited to files.
2. **Known unknowns** — the questions you can name. Resolve one at a time; inspect the repo before asking the user anything code can answer.
3. **Unknown knowns** — tacit taste and intent. Reacting beats imagining: hand over something concrete (a decisions table, a sketch, a sample `.mo` program or Core text) for the user to react to instead of asking them to describe it cold.
4. **Unknown unknowns** — sweep for landmines: limits (`src/base/limits.h`), capability/security boundaries, Lean-vs-C divergence, fuzz surface, upgrade compatibility. Name each with the cheapest probe that would expose it.
5. **Hand over the map** — the completed four-quadrant map with a decision ledger. This is the only done-condition.

## Rules

- Name the current quadrant as you go; finish a stage before opening the next.
- After about 5-6 answered questions, if preferences form a clear pattern, you may offer to answer matching questions on the user's behalf — only on explicit opt-in, each delegated answer disclosed as question → answer → reason, and correctable at any time. Delegation never authorises implementation.
- Every artifact ends with the user's next move pre-drafted (accept/edit/reject options) so reacting costs near-zero typing.

Feed the finished map into `write-spec` for slicing.

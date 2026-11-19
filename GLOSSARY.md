# Mo trust

The words for how a person comes to trust a Mo program that an agent wrote. The language's own
terms (promise, example, anchor, test) are defined in `docs/reference/README.md`, and intent items
and the review door in `docs/design/intent.md`.

## Language

**Check**:
A promise, an example or a test.

**Wrong version**:
A replacement for named declarations of a program, written by an agent to represent one plausible
mistake.
_Avoid_: natural mutant, candidate

**Mutant**:
A copy of a program with one small change that the toolchain made on its own.
_Avoid_: mechanical mutant, mutation

**Survivor**:
A mutant or wrong version that every check of the program still passes.

**Mutation gate**:
The rule that a program's checks fail while one of its mutants survives.

**Behaviour change**:
A check of the previous version of a program that the new version fails.

**Change summary**:
The account of a change in terms of checks: those no longer true, those newly true, those about
something new, and those that could not be compared.

**Shorter run**:
A gated run against the previous version of a program that tries only the mutants of functions
whose code changed or whose checks changed, and carries the rest over untried.

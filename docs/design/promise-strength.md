# Promise strength from natural mutants

Status: Design proposal (16 Oct 2026, task `MO-66` / `S1`). Not yet implemented; implementation is tracked separately under Phase 6 (`MO-67` / `S2`).

## 1. Purpose and scope

Measure how effectively a function's existing promises and examples reject a bounded collection of realistic incorrect implementations.

A score describes the frozen implementations and inputs examined. It is not a proof of correctness, a probability that the program is correct, or a guarantee that the specification captures its author's intent.

`S1` (`MO-66`) prepares this design; `S2` (`MO-67`) implements it separately in Phase 6. Drafting this document does not promote mutation-testing implementation into Phase 2.

## 2. Existing rules this proposal preserves

- Promises contain Boolean conditions plus `allows` and `forbids` anchors (`docs/reference/README.md` §12.2). Examples and tests remain distinct observations (§12.1, §12.3).
- Anchors test the promise body against supplied inputs and results. They do not run the function to manufacture the supplied result (`docs/reference/README.md` §12.2, `docs/design/architecture.md` §10).
- Promise inputs remain boundary-first and cover the types' allowed domain (decision D228). Strength mode introduces no preconditions or "realistic values only" default.
- Test capabilities remain fakes (`docs/reference/README.md` §12.4, decision D234).
- Compiler warnings remain prohibited (decision D223).
- JSON's existing fields retain their names, types, and meanings (decision D233).
- Cancellation, an observed Mo stop, an assertion failure, and tool inability remain distinct outcomes (decision D242).

## 3. What is scored

The unit of measurement is one source function and its unchanged promises and examples.

For each subject, prepare approximately ten distinct natural mutants: plausible wrong implementations with a documented mistake, rather than relying exclusively on mechanical operator changes. Ten is a starting corpus target, not evidence of statistical sufficiency.

Each candidate records:

- Stable identity and source content hash
- Subject identity and original program revision
- Mistake it represents
- Origin and generation method
- Review of whether it genuinely contradicts the intended behavior
- A distinguishing input or other evidence where available
- Eligibility decision and reason

Only the function implementation changes. Its signature, capabilities, promise conditions, anchors, examples, and unrelated declarations remain fixed.

The initial design uses a saved, reviewable corpus. Running strength analysis does not itself contact a model provider, transmit program source, or generate new implementations through an external service. Any later generation service requires a separate design and authorization.

Deduplicate identical candidates. Do not add or remove candidates after seeing their scores without producing a new corpus version.

## 4. Eligibility and the unchanged reference

Before scoring:

1. Check the original program and its anchors.
2. Run the selected original promises and examples.
3. Fix the subject, observation selection, corpus, generator version, ordered input plan, and resource limits.
4. Check each candidate using the normal compiler pipeline.

If the original fails a selected observation, do not publish a strength percentage. Report that the implementation and specification disagree, with the existing failure evidence. The tool cannot determine automatically whether the code or specification is wrong.

The heuristic label "possibly unsound spec" is useful investigation context, but must not replace the existing meaning of a failed promise or classify every reference failure as a specification defect.

Candidates rejected by parsing or checking are invalid candidates, not kills. Duplicates and demonstrably equivalent implementations are excluded with reasons. A surviving candidate is never declared equivalent merely because the selected observations did not distinguish it.

Uncertain candidate validity is reported separately. It must not disappear from the manifest.

## 5. Keep the oracle independent

Freeze the original promise conditions, examples, and their supporting implementation dependencies.

A mutated subject must not change the oracle used to judge it. In particular:

- Mutating the function must not also mutate a helper that computes expected results.
- A promise condition that calls the original subject must not silently call the candidate instead.
- Candidate results and inputs are passed to the unchanged oracle through a validated, type-preserving boundary.
- Unsupported value transfer or oracle isolation is reported as inability, rather than approximated.

This is an architectural requirement for future implementation, not a claim that the current runner provides that separation.

Use fresh execution state and fake capabilities for each independent trial. Do not let candidate state leak into the original, another candidate, or later inputs.

## 6. Outcome model and kill rate

Give each eligible candidate a visible outcome:

- **Killed by promise:** a completed call returns a result for which an unchanged promise condition is false.
- **Killed by example:** an unchanged example fails.
- **Killed by both:** both mechanisms reject it.
- **Survived:** the planned observations complete without either rejection.
- **Runtime defect:** an actual Mo stop occurs; retain its input and defect separately.
- **Inconclusive:** cancellation, infrastructure failure, unsupported generation, or another inability prevents a conclusion.
- **Not run:** planned work was not attempted.

A candidate receives at most one kill in the combined total.

For the primary metric:

- $K$ = candidates killed by promises or examples, counting their union
- $S$ = candidates that survived
- Scored denominator = $K + S$
- Kill rate = $K / (K + S)$

If $K + S$ is zero, the rate is unavailable, not zero or 100%.

Runtime defects do not inflate this primary measure of promise/example strength. Report their number alongside it. Likewise, do not count compiler rejections or timeouts as successful kills.

Always show the full accounting: requested candidates, admitted candidates, exclusions by reason, killed, survived, runtime defects, inconclusive, and not run. A percentage without those counts is insufficient.

Where admitted candidates remain inconclusive or unrun, label the result partial. A high percentage on the resolved subset must not look like complete evidence.

All classification and denominator rules belong to the metric version. Changing them creates a different metric.

## 7. Separate promise contribution from examples

Report three paired views over the same candidate set and observation plan:

1. Promise-only detection
2. Example-only detection
3. Combined detection

The always-true baseline disables promise rejection analytically while preserving examples and all other classification rules. It does not rewrite the program's actual promises or anchors.

This baseline establishes whether the promises add detection beyond the examples. Report the number of additional candidates killed by promises and the percentage-point change on a common denominator.

For each promise, retain which candidates it rejected and the first confirmed false condition. If execution stops after the first false line, later lines are "not evaluated," not "passed." A complete line-contribution matrix requires separately evaluated evidence.

Avoid blanket statements that a promise is weak solely because it adds no kills to a small corpus. The supported statement is that it added no detection on this corpus.

## 8. Anchors are separate evidence

`allows` and `forbids` anchors check whether a promise body accepts or rejects named input/result combinations (`docs/reference/README.md` §12.2, `docs/design/architecture.md` §10). They are not interchangeable with independently generated wrong implementations.

Display anchor conformance separately and preserve compile-time checking. Do not add `forbids` anchors to the natural-mutant denominator: doing so would reward the same hand-picked negatives already required to validate the promise.

An optional contribution view may show which promise conditions reject each `forbids` anchor. It must distinguish directly observed contributions from conditions skipped after an earlier false line.

This preserves anchor contribution reporting while avoiding an inflated implementation score.

## 9. Input plans and limits

Reuse the established boundary-first generation policy (`docs/design/architecture.md` §10). Record the original promise names, generator version, seed derivation, selected input ordinals, and budgets.

Compare candidates using the same fixed ordered input plan where supported. If timing prevents completion, record the completed prefix and the unresolved work. Timing variability must not silently change the denominator or produce a claim of exact reproducibility.

Decision D242 governs promise deadlines (`docs/design/architecture.md` §10.1):

- An interrupted unresolved trial is tool inability, never a kill.
- Cancellation during shrinking retains the last confirmed counterexample.
- A genuine Mo stop remains a genuine stop.
- Clock failure remains tool inability.
- Cooperative cancellation is not hard wall-clock isolation.

Do not claim bounded whole-command execution merely because promise trials have deadlines. Native operations, initialization, and ordinary observations have limitations documented by D242. Whole-run containment must be reviewed before implementing execution of arbitrary generated candidates.

## 10. Proposed command and reporting behavior

The eventual entry point is `mo test --strength`.

Ordinary `mo test` behavior stays unchanged. Strength is explicitly requested analysis.

Selection should retain exact existing observation names and module selection. The implementation design must specify how the selected observations map to subject functions and which additional observations, if any, are required. It must not silently broaden the selection.

Suggested behavior:

- `--list` remains non-executing.
- A low score or surviving mutant does not become a compiler warning (decision D223).
- Strength findings are report data, such as "3 surviving candidates" or "no additional detection beyond examples."
- No arbitrary pass threshold is introduced in the first version.
- A successful complete measurement may exit successfully even when candidates survive.
- Original-program errors, failed original observations, and tool inability retain their existing failure meanings.
- Command mistakes retain exit status 2.

JSON should add explicitly versioned strength record kinds while preserving existing records (decision D233). Required information includes subject, corpus and metric versions, reference status, counts, completeness, paired baseline, exclusions, and reproduction details. Existing test totals must not count a deliberately failing mutant as a failed original test.

Exact new field names and record kinds remain to be finalized against the current serializer and documented before implementation. This design note does not introduce an already-supported JSON contract.

## 11. Illustrative accounting

This is a worked arithmetic example, not an executed result.

For ten admitted candidates:

- Five are killed by promises or examples
- Three survive
- One produces a runtime defect
- One remains inconclusive

The primary rate is $5 / 8$, or $62.5\%$, marked partial.

Suppose promises kill four candidates and examples kill three, with two candidates caught by both. The combined count is five, not seven. The always-true baseline is $3 / 8$, or $37.5\%$; promises add two detections, a 25-percentage-point increase on that common denominator.

The runtime defect and inconclusive candidate remain visible beside the score.

## 12. Future acceptance and regression plan

These are proposed implementation checks for `S2` (`MO-67`):

- A small reviewed fixture produces exact known promise-only, example-only, overlap, and survival counts.
- An unchanged correct implementation produces no false detection.
- A reference failure suppresses the score and preserves its original evidence.
- Invalid, duplicate, equivalent, and unresolved-validity candidates remain visibly accounted for.
- Cancellation never becomes a kill; interrupted shrinking retains the last confirmed witness.
- A stopped candidate is distinguished from a false promise and from tool inability.
- A zero denominator produces an unavailable rate.
- Partial execution cannot display a complete result.
- A corpus detected only by examples reports no extra promise contribution.
- Invalid anchors are rejected before scoring; anchor counts never inflate the natural-mutant rate.
- Oracle dependencies remain unchanged when the candidate executes.
- Fake state is isolated across trials and candidates.
- Existing selection behavior and non-executing listing are preserved.
- Existing JSON records and totals retain their meanings.
- Replaying a fixed corpus and completed input plan reproduces classifications; timing differences are disclosed.
- The document explicitly retains the Phase 6 implementation boundary.

## 13. Decisions, alternatives, and reversal

All choices below are proposed for the design, not yet implemented in the compiler or test runner:

1. **Saved natural-mutant corpus.**
   - *Alternative:* generate candidates live during every run.
   - *Reason:* reviewability and reproducible evidence.
   - *Reversal:* add a separately authorized generation stage while retaining versioned saved inputs.
2. **Runtime defects separate from primary strength.**
   - *Alternative:* count every observed failure as a kill.
   - *Reason:* distinguish the oracle's strength from runtime safety checks.
   - *Reversal:* add a separately named broader detection metric; do not change the original metric's meaning.
3. **Anchors separate from the denominator.**
   - *Alternative:* score hand-picked forbidden outputs alongside implementation candidates.
   - *Reason:* prevent mandatory anchor conformance from inflating mutation evidence.
   - *Reversal:* publish a separate anchor score with its own label.
4. **Informational opt-in findings without a numerical gate.**
   - *Alternative:* a threshold that fails the command.
   - *Reason:* no supported threshold or approved warning behavior exists.
   - *Reversal:* introduce an explicit threshold policy later with documented semantics.
5. **Neutral reference/spec disagreement.**
   - *Alternative:* label every original failure "possibly unsound spec."
   - *Reason:* the tool cannot independently assign blame.
   - *Reversal:* add a reviewed-reference designation and evidence, without altering ordinary promise failures.
6. **Frozen original oracle.**
   - *Alternative:* compile the whole candidate program and allow its changes to affect oracle calls.
   - *Reason:* an oracle that changes with the subject can reward the mistake it should detect.
   - *Reversal:* requires an explicit revised attribution model and new metric version.

## 14. Dependencies and scope boundary

`S1` (`MO-66`) has no task prerequisites. `S2` (`MO-67`) depends on scalar promise execution (`I1` / `MO-44`) and `S1`, and remains scoped to Phase 6.

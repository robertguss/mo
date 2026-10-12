# Final round, Claude Code family: grading C, job-queue

Grader C, a fresh session, settling job-queue: grader A gave `pass`, grader B
`close`. Judged against `docs/reference/README.md` (v0.1) alone, by reading,
following `learnability/rubric.md`. "L141" means line 141 of `job-queue.mo`.
Section numbers are the reference's. "Parses" comes from the executable grammar.

## 9. job-queue (sample)

| Parses | Would compile | Does the task | Verdict |
| --- | --- | --- | --- |
| yes | no, one mistake. L141, `result is Error(BadLength(_)) implies ...`, where L64 declares `BadLength(length: Duration)`. A case carries "no data, one unnamed value, or named fields", and is built "by naming its fields, or from its one unnamed value" (4.2). Patterns follow the same split (7.3): `Circle(p)` matches "that case, with its value matching `p`", meaning its one unnamed value, and a case with named fields is matched by naming them, as in `Rectangle(width: p, height: q)`. 7.2 matches the one-field case `UnknownItem(name: Text)` the same way, as `Error(UnknownItem(name: n))`. 7.2 lets `_` "stand for anything" in a pattern's slot, but that doesn't give `BadLength` an unnamed slot. No rule gives `BadLength(_)` a meaning, so a compiler following the reference rejects it. The grammar accepting it (Appendix B) only means it parses. Section 14's prose `Error(NotAllowed(path))`, for `NotAllowed(path: Text)`, describes a returned value. It isn't a pattern rule and doesn't override 4.2 and 7.3. Grader B is right, and grader A missed this. Nothing else, beyond B's checks: every record and case is built with every field named (4.1, 4.2). No case reuses an always-in-scope name such as `Failed` or `NotFound` (4.2, 10.5). Names bound by `is` in a promise line are visible only after that line's `implies` or `and` (7.4), so reusing `after` and `held` on later lines rebinds nothing (6.1). `(-5).seconds` negates the `Int` (Appendix B precedence). The random test's `case` over an `Int` has `Unit` arms (6.1, 7.2). Each of the 36 anchors in the 12 promises is accepted or rejected as its keyword says (12.2). The promises hold on queues built through the exposed functions (12.2), where a ready job has had fewer than three tries. | yes: `submit`; `lease` gives the oldest ready job from `now` for a `Duration` and counts the try; `acknowledge`, `fail` and `expire`; a job's third failed or expired try leaves it `Dead`; the queue is a value with hidden insides; every operation that changes the queue has a "no job is lost" promise (`keeps_every_job?`, and its own one for `take_finished`); the 7 tests and the examples hold by reading. | **close** (does the task; one compile mistake) |

Stumbles:

- `BadLength(_)` (L141) for the case `BadLength(length: Duration)`: a positional pattern on a case with a named field, which is matched by naming it (7.3, 4.2). This is likely the positional `Error(_)` and `Some(_)` form of 7.2, applied to a named-field case. The file writes `Leased(until: _)` (L411) and `BadLength(length: length)` (L174) correctly, so it's a slip.

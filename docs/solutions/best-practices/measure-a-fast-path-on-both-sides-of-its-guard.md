---
title: Measure a fast path on both sides of its guard
date: 2026-11-17
category: best-practices
module: lib/json.mo
problem_type: best_practice
component: testing_framework
severity: medium
applies_when:
  - A change adds a fast path that a guard turns on or off for a whole input
  - One fixture decides whether a speed bar is met
  - The fixture was written before the fast path existed
tags: [performance, benchmark, fixture, fast-path, json, review]
---

# Measure a fast path on both sides of its guard

## Context

Phase 7's design step had a bar: fetch and parse 1 MiB of JSON in 2.0 seconds on the debug
build. `Json.parse` was made faster by reading a string with no escape as one piece between two
quotes.

The first version kept a list of the special characters the whole text holds. When the list was
empty, the check of each string was skipped. The body that the bar measures holds no backslash
and no control character, so its list was empty. The body took 28,250,591 instructions, and the
record said the bar was met.

The review then changed one string of that body to hold an escaped quote. The body took
34,040,701 instructions, 20% more. A real API response nearly always holds an escape somewhere,
so the recorded number was the best case and no real response would get it.

The fixture was written before the fast path existed. Nobody chose it to flatter the change. It
only happened to be on the cheap side of the guard.

## Guidance

When a change adds a fast path with a guard, look at the fixture that measures it and ask which
side of the guard the fixture is on.

- Measure one input on each side of the guard, and record both. Change the fixture as little as
  possible to cross the guard: one escape in one string was enough here.
- Prefer a design whose cost does not depend on a property of the whole input. The fix checks
  each piece for a backslash directly. The body then takes 28,842,519 instructions with no
  escape and 28,878,910 with escapes in one item. That is 2% more than the first version's best
  case and 15% less than its usual case.
- Count instructions, not seconds, for this comparison. The host was busy during the review and
  wall times were not usable. The instruction count was the same on every run.
- Also measure the smallest input. The new parser pays about 1,700 instructions for each parse
  before it reads anything, so a document under about 25 bytes got slower.

## Why This Matters

A bar met only on the fixture's best case is a claim about the fixture, not about the code. The
design document told Robert the bar was met, and one decision of his rested on it. The margin
after the fix is 6% under the bar. With the first version, a real response would have been near
the bar or over it, and nothing in the record would have said so.

## When to Apply

Any speed claim made from one fixture: a parser, an encoder, a lookup with a cached case. The
evidence and the measured cases for this one are in
`learnability/phase7/preflight/json-parse/README.md`.

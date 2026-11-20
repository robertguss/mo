---
title: Mo code costs by instruction count, so keep a hot function small
date: 2026-11-17
category: best-practices
module: lib
problem_type: best_practice
component: tooling
severity: medium
applies_when:
  - Library code in Mo runs once for each character, byte or item of an input
  - A Mo program is too slow and the cause is not known
  - A parser or a protocol is written in Mo, such as the HTTP client of Phase 7
  - A reader in Mo is called again each time more input arrives
tags: [performance, vm, bytecode, instructions, parser, lib, mo]
---

# Mo code costs by instruction count, so keep a hot function small

## Context

`Json.parse` took 109,677,820 instructions for a 1 MiB document, about 105 for each byte. A
throwaway counter of instructions for each function showed where they went, and the parser now
takes 28,842,519. The counter and the build that held it are not kept. These facts were measured
on 2026-11-17 on the debug build, and they describe the code the compiler writes today. The
phase about speed can change them.

## Guidance

- **The VM's time follows the number of instructions.** The debug build ran 16 to 20 million a
  second on every workload measured. Count instructions to compare two versions: the count is
  the same on every run, and wall time on a shared host is not.
- **A call costs by the size of the function, not by what the call does.** A `while true` loop
  around a `case` ran 116 instructions for each call, 62 of them `const`, even when it returned
  at once. A function of one comparison ran 23. So put the usual case in a small function, and
  call the large one only when needed:

  ```mo
  fn skip_space(chars: List(Char), at: Int) : Int
    if (chars.get(at) or 'x').code > 32
      at
    else
      skip_spaces(chars, at)
    end
  end
  ```

- **A `for` loop over a list ran about 62 instructions for each item**, 28 of them `clear`. A
  loop inside a function that runs once for each string or token is the first thing to remove.
- **One primitive call replaces a loop.** `text.split`, `text.contains?` and `Text.from_chars`
  do in one instruction what a loop does in hundreds. The parser's gain came from cutting the
  text at its quotes with one `split` and taking each plain string whole.
- **A structure built for each input has a fixed cost.** The parser now looks for 32 control
  characters before it reads anything. That is about 1,700 instructions for each parse, and a
  document under about 25 bytes got slower.
- **Measure how the input arrives, not only how large it is.** The HTTP client's reader ran
  again from the first byte after each receive, and its 1 MiB case met its bar, because 1 MiB
  arrives in a few large receives. A body of 3,000 chunks that arrived one at a time used a
  whole CPU for as long as it took, because each receive read every earlier chunk again. A
  fake network gives a whole reply in one receive, so no test against a fake shows this. A
  reader that is called again as input arrives needs one measurement where the input comes
  in many small pieces, and a test that reads it a piece at a time
  (`learnability/phase7/preflight/http-in-mo/README.md`, "The client as built").

## Why This Matters

Without a counter for each function, the first two attempts at the parser were guesses. An
estimate of 23 million instructions for one design gave 60 million when measured, because the
cost of the helper functions was not known. With these facts a parser or a protocol in Mo can
be written near its final speed the first time.

## When to Apply

When writing or reviewing Mo code under `lib/` or `runtime/` that runs for each character or
item of an input. The measured versions of the parser are in
`learnability/phase7/preflight/json-parse/README.md`. The patch that prints one run's
instruction count is `learnability/phase7/preflight/http-in-mo/prototype.patch`
(`MO_PROTOTYPE_STATS`).

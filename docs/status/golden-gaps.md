# Golden corpus: what the language reference defines and `mo` does not yet provide

Found while writing `tests/golden/`. Each program avoids these forms, so the corpus passes today;
a form that lands should gain a golden program. Section numbers are Language Reference v0.1.

## Coverage indexed by reference section

`tests/conformance/rules.tsv` lists remaining partial coverage by reference section, including
missing boundary and refusal programs. It is a map with a guard, not proof of conformance or
completion of the Phase 2 conformance item. A gap there is not a pass.

The native BigInt functions, Env and Random fakes, Ordering comparison, qualified `Int.compare`,
and Ordering, Entry and Map text now have golden programs in `ref-bigint`,
`ref-capability-fakes` and `ref-abilities`. `ref-library-natives` also covers `List.to_set` and
`Int.to_float`. Direct calls on a nullary case, such as `Equal.to_text` and
`Less.compare(Greater)`, are in `lang-dot-calls-cases`, and a `case` written across several lines
inside interpolation is in `txt-interpolation-case`. Their former unsupported entries must not be
retained as language limitations.

## Refused by the reference, accepted by the checker

Reference 5.4 says to write `x.f`, not `x.f()`, and appendix C lists `x.f()` as a refused
spelling. On main `fc53746`, `mo check` accepts `3.twice()`, `[1, 2].size()` and
`"ab".trim().byte_count()`, and `mo run` runs them. MO-141 tracks the refusal. Until it lands,
no fixture pins `x.f()`.

## How many U+FFFD a run of invalid input bytes becomes

Reference 14 says each sequence of bytes on standard input that isn't valid UTF-8 "becomes one
U+FFFD". Read plainly, the line `a`, FF, FE, `b` reads as `a�b`, three characters. `mo` follows
Unicode's maximal-subpart rule (architecture 7, `src/host/console.c`) and gives `a��b`, one U+FFFD
for each of FF and FE, and three for the encoded surrogate ED A0 80. Robert decides which reading is
meant; then either the reference or `console.c` changes. `cap-console-stdin-invalid` uses only
inputs where both readings agree: a lone invalid byte, a truncated sequence, and a lead byte
followed by ASCII.

## Other

- A choice case cannot be named `Ok` or `Failed` (E0043), and number literals with exponents such
  as `1e21` are rejected (E0004); both match the reference as written.
- The bug report printed on exit 70 is pinned by the `fail-bug-*`, `col-take-negative` and
  `lang-stop` cases, though reference 13.2 does not fix its wording. Change those goldens in the
  same PR as any change to the report format.

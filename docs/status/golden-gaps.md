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

## Qualified `to_text` and `compare`

`docs/status/section-14-inventory.md` records a probe of every section 14 function. Every one
runs as a dot call, including on a nullary case receiver (`lang-dot-calls-cases`, and
`ref-case-receivers`). Written through a type or module, `to_text` and `compare` work only as
`Int.compare` and the scalar `to_text` primitives; `Text.compare(a, b)`, `List.to_text(xs)` and
the rest report E1103. D285 makes that form part of the language for every type with the
ability, and MO-144 builds it. These failures are not expected-error fixtures.

## Refused by the reference, accepted by the checker

A specification followed directly by its code, with no blank line, is accepted, and
`mo format` leaves it unchanged. Reference 12 places a blank line there but does not say whether
leaving it out is refused or formatted.

## Refused with a placeholder or a misleading hint

Making a fake capability, or reading `printed` or `printed_errors`, outside a test, example or
promise is an error (reference 11.4 and 12.4). `mo check` refuses each one with E1103, "this body
rule is not implemented by the staged checker", not a dedicated diagnostic. E1103 marks an
unfinished form, so it is not an expected-error fixture.

These spellings from appendix C are refused, but the error points away from the replacement, so
no fixture pins them:

- `x => x + 1` as an argument reports E0016, "a case arm is written Pattern: value, with no =>",
  as if it were a case arm. The lexer reports `=>` the same way wherever it appears.
- `|x| x + 1` reports E0007, "a value is an expression", citing statement separation (2.4).
- A postfix `!`, as in `items.first!`, reports E0025, "Mo writes not instead of !", which points
  to `not`, not to `case` with `stop` (8.4).

`lambda x: ...` (`e0021-lambda`) and `unwrap()` (`e0050-unwrap`) are pinned.

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

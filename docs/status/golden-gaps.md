# Golden corpus

## Coverage indexed by reference section

`tests/conformance/rules.tsv` is a map with a guard, not proof of conformance or
completion of the Phase 2 conformance item. A gap there is not a pass.

The native BigInt functions, Env and Random fakes, Ordering comparison, qualified `Int.compare`,
and Ordering, Entry and Map text now have golden programs in `ref-bigint`,
`ref-capability-fakes` and `ref-abilities`. `ref-library-natives` also covers `List.to_set` and
`Int.to_float`. Direct calls on a nullary case, such as `Equal.to_text` and
`Less.compare(Greater)`, are in `lang-dot-calls-cases`, and a `case` written across several lines
inside interpolation is in `txt-interpolation-case`. Qualified `T.to_text` and `T.compare`, as
calls and as values, have `ref-qualified-abilities` (D285). Their former unsupported entries must
not be retained as language limitations. Invalid UTF-8 on standard input gives one U+FFFD per
maximal invalid subpart, as reference 14 now says (D291), and `cap-console-stdin-invalid` pins
inputs where that differs from one U+FFFD per run. Since MO-140, no `rules.tsv` row has a gap.

## Other

- A choice case cannot be named `Ok` or `Failed` (E0043), and number literals with exponents such
  as `1e21` are rejected (E0004); both match the reference as written.
- The bug report printed on exit 70 is pinned by the `fail-bug-*`, `col-take-negative` and
  `lang-stop` cases, though reference 13.2 does not fix its wording. Change those goldens in the
  same PR as any change to the report format.

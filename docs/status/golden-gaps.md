# Golden corpus: what the language reference defines and `mo` does not yet provide

Found while writing `tests/golden/`. Each program avoids these forms, so the corpus passes today;
a form that lands should gain a golden program. Section numbers are Language Reference v0.1.

## Checked and run, but no golden program yet

- `List`: `filter`, `filter_map`, `flat_map`, `any?`, `group_by` (reference 14).
- `List.to_set` and Set text forms (reference 14 and 9.2) run in `lib/set.mo` and
  `tests/lower-source/set/main.mo`.
- `Int`: `floor_div`, `floor_mod`, `pow`, `checked_sub`, `checked_mul`, `wrapping_*`,
  `saturating_*`, `to_big`, `to_byte`, `to_float`; `Float`: `sqrt`, `floor`, `ceil`,
  `round`, `abs`, `nan?`, `infinite?`, `to_decimal`; `Bytes`: `size`, `get`, `decode_utf8`.
  The `int-natives`, `float-natives` and `bytes-natives` lower-source fixtures cover these.

## Not lowered yet (E1150)

- `Int.compare`. `BigInt`: `to_int`, `abs`, `min`.
- `List.sum`.
- `Env.get`, `Random.int`.
- Interpolating an `Ordering`, an `Entry`, or a `Map` (derived text shape).

## Other

- `use Unicode16` fails with E0034: there is no `lib/` folder yet.
- A choice case cannot be named `Ok` or `Failed` (E0043), and number literals with exponents such
  as `1e21` are rejected (E0004); both match the reference as written.
- The bug report printed on exit 70 is pinned by the `fail-bug-*`, `col-take-negative` and
  `lang-stop` cases, though reference 13.2 does not fix its wording. Change those goldens in the
  same PR as any change to the report format.

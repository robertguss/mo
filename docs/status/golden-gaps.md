# Golden corpus: what the language reference defines and `mo` does not yet provide

Found while writing `tests/golden/`. Each program avoids these forms, so the corpus passes today;
a form that lands should gain a golden program. Section numbers are Language Reference v0.1.

## Not lowered yet (E1150)

- `Int`: `floor_div`, `floor_mod`, `pow`, `checked_*`, `wrapping_*`, `saturating_*`, `to_big`,
  `to_byte`, `to_float`, `compare`. `BigInt`: `to_int`, `abs`, `min`.
- `Float`: `sqrt`, `floor`, `ceil`, `round`, `abs`, `nan?`, `infinite?`, `to_decimal`.
- `Text.compare`; `Bytes.size`, `Bytes.decode_utf8`.
- `List.sum`, `List.to_set`.
- `Env.get`, `Random.int`.
- Interpolating an `Ordering`, an `Entry`, or a `Map` or `Set` (derived text shape).

## Other

- `use Unicode16` fails with E0034: there is no `lib/` folder yet.
- A choice case cannot be named `Ok` or `Failed` (E0043), and number literals with exponents such
  as `1e21` are rejected (E0004); both match the reference as written.
- The bug report printed on exit 70 is pinned by the `fail-bug-*`, `col-take-negative` and
  `lang-stop` cases, though reference 13.2 does not fix its wording. Change those goldens in the
  same PR as any change to the report format.

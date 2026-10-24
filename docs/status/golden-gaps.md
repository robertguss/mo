# Golden corpus: what the language reference defines and `mo` does not yet provide

Found while writing `tests/golden/`. Each program avoids these forms, so the corpus passes today;
a form that lands should gain a golden program. Section numbers are Language Reference v0.1.

## Checked and run, but no golden program yet

- `List.to_set` (reference 14) runs in `lib/set.mo` and `tests/lower-source/set/main.mo`.
- `Int.to_float` runs in the `int-natives` lower-source fixture.

`ref-library-natives` now covers the previously listed List functions, Int arithmetic natives,
Float natives, Bytes operations and `Byte.to_int`. `ref-abilities` covers `List.sum` at all four
numeric types and Set text. `tests/conformance/rules.tsv` lists the remaining partial coverage
by reference section, including missing boundary and refusal programs. A gap there is not a pass.

## Not lowered yet (E1150)

- `BigInt`: `to_int`, `abs`, `min`.
- `Env.get`, `Random.int`.
- Interpolating an `Ordering`, an `Entry`, or a `Map` (derived text shape).
- Comparing two `Ordering` values through bound names, such as
  `a = Equal`, `b = Greater`, `a.compare(b)`. Reference 9.2 requires `Less`.
  Calling `Equal.compare(Greater)` directly instead reports E1103 in the staged checker.

`Int.compare` works and is covered by `ref-abilities`; its former entry here was stale.

## Accepted by the reference, rejected by the parser

Reference 3.4 allows any expression inside interpolation. This complete program should print
`one` and exit 0, but `mo check` reports E0016 at the first arm, followed by parser recovery errors:

```mo
module Main

fn main(console: Console)
  console.print("#{case 1
    1: "one"
    _: "other"
  end}")
end
```

Computing the `case` in a binding before interpolating it works, as `ref-operators` does.
The parser's error is not a conformance expectation.

## Other

- A choice case cannot be named `Ok` or `Failed` (E0043), and number literals with exponents such
  as `1e21` are rejected (E0004); both match the reference as written.
- The bug report printed on exit 70 is pinned by the `fail-bug-*`, `col-take-negative` and
  `lang-stop` cases, though reference 13.2 does not fix its wording. Change those goldens in the
  same PR as any change to the report format.

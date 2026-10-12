# Core text, version 0.1

The Lean implementation in `MoCore/Text/` is the reference codec. Every file
is `(core 0.1 PAYLOAD)` followed by LF. Constructors are case-sensitive and
named after Lean. A nullary constructor is still a list: `(unit)`.

The initial codec covers types and literals; the program, run and outcome
grammars below will be completed with the runner.

Canonical S-expressions have one ASCII space between elements, no internal
line breaks, and no space just inside parentheses. Strings use `\"`, `\\`,
`\n`, `\r`, `\t`; other controls U+0000–001F and U+007F use `\u{hex}` with
lowercase shortest hex. Other Unicode scalars print literally, without
normalization. Input accepts 1–6 hex digits (either case) in `\u{...}` and
rejects surrogates and values above U+10FFFF. Raw controls are rejected.

Whitespace is space, tab, LF or CR. `;` comments run through LF or EOF and
act as whitespace outside strings. Atoms end at whitespace, parentheses,
double quote or semicolon. Decimal whole numbers have arbitrary precision,
optional minus, and at least one ASCII digit; no plus or separators. Input
allows leading zeros and negative zero; printing removes them. Naturals must
be nonnegative. Float bits are `0x` and exactly 16 hex digits; input allows
either case, printing uses lowercase and preserves every bit, including NaN
payloads and negative zero. Names are always strings, never symbols.

Parsing is not type checking: an out-of-range integer/byte literal can be
parsed, then refused by the checker.

```ebnf
file = "(", "core", "0.1", payload, ")" ;
sexp = atom | string | "(", { sexp }, ")" ;
integer = [ "-" ], digit, { digit } ;
bits = "0x", hex-digit (* exactly sixteen *) ;
type = "(int)" | "(bigInt)" | "(byte)" | "(decimal)" | "(float)"
     | "(bool)" | "(char)" | "(text)" | "(bytes)" | "(unit)"
     | "(range)" | "(time)" | "(duration)" | "(ordering)" | "(fileError)"
     | "(tuple (", { type }, "))" | "(list ", type, ")"
     | "(map ", type, type, ")" | "(set ", type, ")"
     | "(data ", string, ")" | "(option ", type, ")"
     | "(result ", type, type, ")" | "(fn (", { type }, ") ", type, ")"
     | "(cap ", capability, ")" ;
capability = "(console)" | "(files)" | "(readFiles)" | "(clock)"
           | "(random)" | "(args)" | "(env)" ;
literal = "(", numeric-tag, integer, ")" | "(float ", bits, ")"
        | "(bool ", ("true" | "false"), ")" | "(char ", string, ")"
        | "(text ", string, ")" | "(bytes (", { integer }, "))" | "(unit)" ;
numeric-tag = "int" | "bigInt" | "byte" | "decimal" | "time" | "duration" ;
```

Decimal integers count 10⁻¹⁸ units; Time and Duration integers count
microseconds. Char strings contain exactly one Unicode scalar.

```text
(core 0.1 (map (text) (list (byte))))
(core 0.1 (float 0x8000000000000000))
(core 0.1 (text "hello\nβ"))
```

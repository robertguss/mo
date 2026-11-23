# Where the library is implemented

This is a map of Mo's core library: every function
of reference section 14, and every operator that section 3 defines on a
built-in type, with where it is implemented. It belongs to Architecture v0.1
(`architecture.md`) and points into two of its contracts, `src/base/prims.def`
and `src/base/hostops.def` (`bytecode.md` sections 6 and 7). `make
library-map` checks that this map and those two tables agree, and that each
primitive's Core route exists in both C and Lean's codec tables. Arithmetic
instruction routes are checked too. The explicit lowering recipes and the
two existing unmodeled quoted-text helpers are listed in
`formal/TRANSLATION.md`; a new primitive without a Core route fails the check.

The runtime-owned store operations in the `STOREOP` section of `hostops.def`
are not library functions. `LOAD` reads a snapshot and consecutive messages,
`APPEND` commits the next message, and `SNAPSHOT` replaces state and prunes the
covered log. `host/store.c` implements all three for disk and in-memory SQLite.
The separate `FAILUREOP` table names typed capture, listing, bounded metadata catalog, indexed pair lookup, export and summary operations. Capture atomically writes a diagnostic pair and retention changes, reporting committed, clean refusal or unsafe outcome; none changes recovery state.
No handler receives the store capability, and no checker signature, Core form
or bytecode operation exposes it. See [saved state](saved-state.md).

The runtime-only `LISTENEROP` section of `hostops.def` is not a library family
either. `NOW`, `WAIT`, `ACCEPT`, `READ`, `WRITE`, `AUTH`, `HALF_CLOSE` and `CLOSE`
are the operations the private MCP server calls on its listener. `host/listener.c`
implements all eight for real sockets and its fake. No handler receives the listener,
and it has no Mo type, checker signature, Core form or bytecode operation. See
[services](services.md).

The `SIMOP` section is VM-handled, not host-dispatched. Those rows carry no
fixed Mo signature: a checked service/tool selector supplies the types.
The map prefixes their row IDs with `SIM_` to distinguish this family.

## The rule

The library is written in Mo wherever it can be (condition 4 of D207). C
implements only what Mo can't express or must be fast: arithmetic and
conversions, text and byte operations, the basic list, map and set
operations, and the capability operations (architecture section 7). Every
function that takes a function as a parameter is Mo code in `lib/`, so C never
calls back into Mo (`bytecode.md` section 6).

Two facts settle most of the choices that rule leaves open:

- **Order is Mo code.** The order of a record, a choice type, a tuple or a list
  is a `compare` function that lowering generates (architecture 4.3,
  `bytecode.md` section 8). So everything that sorts or picks the smallest
  element is Mo code, `sort`, `min` and `max` as well as `sort_by`: a
  primitive would have to call `compare` back.
- **Equality and hashing are not.** The VM compares and hashes any value
  structurally, without running Mo code (`bytecode.md` section 8). So
  `contains?`, `unique`, `to_set`, and the map and set operations, set algebra
  included, are primitives.

## The six places

| Where | Implemented as | The third column gives |
| --- | --- | --- |
| instruction | an instruction of `bytecode.md` section 4.4, on immediates | the instruction family, and the type it works at |
| primitive | C in `src/prim/`, called by the `prim` instruction (`bytecode.md` 4.6) | its id in `prims.def` |
| host | a capability operation in `src/host/`, called by the `host` instruction (`bytecode.md` 7) | its id in `hostops.def` |
| generic | the VM's structural equality and hashing, driven by the type table (`bytecode.md` 8) | what runs it |
| lowering | Core that lowering writes in place, or generates for each type (architecture 4.3) | what it becomes |
| Mo | Mo code in `lib/` | its file |

The fourth column gives the reason for a choice that the rule doesn't make by
itself.

Lowering also emits primitives that implement no library name. Their names
start with `%`, which no Mo name can (reference 2.3), and they sit in their
types' sections below: `List.%empty`, `Map.%empty` and `Map.%literal_insert`
for literals; `Text.%concat` for interpolation; `Text.%quoted` and
`Char.%quoted` for text forms; `Text.%compare`, `BigInt.%compare` and
`Bytes.%compare` for order; `BigInt`'s arithmetic operators; and
`Text.%structural`, the VM's structural text form, for the details of a
failed assertion.

## How `make library-map` checks it

`ci/library_map.sh` reads the tables below. Each section is about one type,
whose module is the first word of its heading. A row whose second column is
`primitive` or `host` names exactly one id in its third column, in backticks.
That id must be a line of `prims.def` or `hostops.def`, in the section's module,
and when the row's first column starts with a function's name, the line has
that name. Every line of the two tables must have exactly one such row. A `Mo`
row names its file, which reference 2.1 names after the module: `FileError`'s
is `lib/file-error.mo`. Any other id in backticks must be a line of one of the
tables, the second column must be one of the six places, and no function has
two rows in a section.

## Every type

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| hashing, for map keys and set members | generic | the map and set primitives, with SipHash-1-3 (`bytecode.md` 8) | |
| `x.compare(y)` (D232), the order `sort`, `sort_by`, `sort_by_desc`, `min` and `max` use, and `<` on tuples, lists, records and choice types | lowering | generated for each type with `Order`: the type's own `compare` where it has one; for the immediates of `bytecode.md` 4.4, `lt` and `eq`, except for `Float`, whose order is `cmp_float`, with not-a-number last; `Equal` for `Unit`; `Text.%compare`, `BigInt.%compare` and `Bytes.%compare`; tuples part by part from the left, lists element by element, records field by field and choice types case by case, in declaration order | the text forms and the order programs see are Mo functions, not VM operations (`bytecode.md` 8). Mo code in `lib/` orders with `compare`, not `<`, because `<` on floats follows IEEE 754 and leaves not-a-number unordered |
| `to_text`, at a type without its own, and `#{x}` in a text | lowering | generated for each type with `Text`, in the form that looks like code (reference 9.2): from the built-in types' `to_text`, with `Text.%quoted` and `Char.%quoted` for texts and characters inside another value, joined with `Text.%concat` | as for `compare` |

## Bool

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a and b`, `a or b` | lowering | a `branch`: the right side is computed only when the left doesn't decide | no function can skip computing an argument (reference 13.1) |
| `a implies b` | lowering | `not a or b`, as a `branch` | as for `and` |
| `not a` | instruction | `not` | |
| `a == b`, `a != b` | instruction | `eq` at `bool`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | instruction | `lt` and `le` at `bool`, `false` first; `>` and `>=` swap the operands | |
| `to_text` | lowering | `"true"` or `"false"`, by a `branch` | |

## Int

`bytecode.md` 4.4: numeric functions other than the operators and generated
`compare` are primitives.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a + b`, `a - b`, `a * b` | instruction | `add`, `sub` and `mul` at `int` | |
| `a / b`, `a % b` | instruction | `div` and `rem` at `int` | |
| `-a` | instruction | `neg` at `int` | |
| `a == b`, `a != b` | instruction | `eq` at `int`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | instruction | `lt` and `le` at `int`; `>` and `>=` swap the operands | |
| `compare(b)`, also `Int.compare(a, b)` | lowering | native Order lowering, as for every type with Order; qualified `T.compare` and `T.to_text` lower through the ability proof (D285) | no new primitive |
| `abs` | primitive | `INT_ABS` | |
| `min(b)` | primitive | `INT_MIN` | |
| `max(b)` | primitive | `INT_MAX` | |
| `pow(n)` | primitive | `INT_POW` | |
| `floor_div(b)` | primitive | `INT_FLOOR_DIV` | |
| `floor_mod(b)` | primitive | `INT_FLOOR_MOD` | |
| `wrapping_add(b)` | primitive | `INT_WRAPPING_ADD` | |
| `wrapping_sub(b)` | primitive | `INT_WRAPPING_SUB` | |
| `wrapping_mul(b)` | primitive | `INT_WRAPPING_MUL` | |
| `saturating_add(b)` | primitive | `INT_SATURATING_ADD` | |
| `saturating_sub(b)` | primitive | `INT_SATURATING_SUB` | |
| `saturating_mul(b)` | primitive | `INT_SATURATING_MUL` | |
| `checked_add(b)` | primitive | `INT_CHECKED_ADD` | |
| `checked_sub(b)` | primitive | `INT_CHECKED_SUB` | |
| `checked_mul(b)` | primitive | `INT_CHECKED_MUL` | |
| `to_decimal` | primitive | `INT_TO_DECIMAL` | |
| `to_float` | primitive | `INT_TO_FLOAT` | |
| `to_big` | primitive | `INT_TO_BIG` | |
| `to_byte` | primitive | `INT_TO_BYTE` | |
| `to_char` | primitive | `INT_TO_CHAR` | |
| `to_text` | primitive | `INT_TO_TEXT` | |
| `seconds` | primitive | `INT_SECONDS` | `second` resolves to the same primitive |
| `milliseconds` | primitive | `INT_MILLISECONDS` | `millisecond` resolves to the same primitive |
| `minutes` | primitive | `INT_MINUTES` | `minute` resolves to the same primitive |
| `hours` | primitive | `INT_HOURS` | `hour` resolves to the same primitive |
| `days` | primitive | `INT_DAYS` | `day` resolves to the same primitive |

## BigInt

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a + b` | primitive | `BIGINT_ADD` | `bytecode.md` 4.4's instructions take immediates, and a `BigInt` is a reference |
| `a - b` | primitive | `BIGINT_SUB` | as for `+` |
| `a * b` | primitive | `BIGINT_MUL` | as for `+` |
| `a / b` | primitive | `BIGINT_DIV` | as for `+` |
| `a % b` | primitive | `BIGINT_REM` | as for `+` |
| `-a` | primitive | `BIGINT_NEG` | as for `+` |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | primitive | `BIGINT_COMPARE`, then `eq` on the `Ordering` it gives | as for `+` |
| `abs` | primitive | `BIGINT_ABS` | |
| `min(b)` | primitive | `BIGINT_MIN` | |
| `max(b)` | primitive | `BIGINT_MAX` | |
| `pow(n)` | primitive | `BIGINT_POW` | |
| `floor_div(b)` | primitive | `BIGINT_FLOOR_DIV` | |
| `floor_mod(b)` | primitive | `BIGINT_FLOOR_MOD` | |
| `to_int` | primitive | `BIGINT_TO_INT` | |
| `to_text` | primitive | `BIGINT_TO_TEXT` | |

## Byte

`Byte` has no arithmetic (reference 3.2).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a == b`, `a != b` | instruction | `eq` at `byte`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | instruction | `lt` and `le` at `byte`; `>` and `>=` swap the operands | |
| `to_int` | primitive | `BYTE_TO_INT` | |
| `to_text` | primitive | `BYTE_TO_TEXT` | |

## Decimal

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a + b`, `a - b`, `a * b` | instruction | `add`, `sub` and `mul` at `decimal` | |
| `a / b`, `a % b` | instruction | `div` and `rem` at `decimal` | |
| `-a` | instruction | `neg` at `decimal` | |
| `a == b`, `a != b` | instruction | `eq` at `decimal`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | instruction | `lt` and `le` at `decimal`; `>` and `>=` swap the operands | |
| `abs` | primitive | `DECIMAL_ABS` | |
| `min(b)` | primitive | `DECIMAL_MIN` | |
| `max(b)` | primitive | `DECIMAL_MAX` | |
| `round(places)` | primitive | `DECIMAL_ROUND` | |
| `floor` | primitive | `DECIMAL_FLOOR` | |
| `ceil` | primitive | `DECIMAL_CEIL` | |
| `to_int` | primitive | `DECIMAL_TO_INT` | |
| `to_float` | primitive | `DECIMAL_TO_FLOAT` | |
| `to_fixed(places)` | primitive | `DECIMAL_TO_FIXED` | |
| `to_text` | primitive | `DECIMAL_TO_TEXT` | |

## Float

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a + b`, `a - b`, `a * b` | instruction | `add`, `sub` and `mul` at `float` | |
| `a / b`, `a % b` | instruction | `div` and `rem` at `float` | |
| `-a` | instruction | `neg` at `float` | |
| `a == b`, `a != b` | instruction | `eq` at `float`, and `not` for `!=`: IEEE 754, so not-a-number equals nothing | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | instruction | `lt` and `le` at `float`; `>` and `>=` swap the operands. IEEE 754 leaves not-a-number unordered, so `sort`, `min` and `max` use `cmp_float` instead, which puts it last | |
| `abs` | primitive | `FLOAT_ABS` | |
| `floor` | primitive | `FLOAT_FLOOR` | |
| `ceil` | primitive | `FLOAT_CEIL` | |
| `round` | primitive | `FLOAT_ROUND` | |
| `sqrt` | primitive | `FLOAT_SQRT` | |
| `nan?` | primitive | `FLOAT_IS_NAN` | |
| `infinite?` | primitive | `FLOAT_IS_INFINITE` | |
| `to_decimal` | primitive | `FLOAT_TO_DECIMAL` | |
| `to_text` | primitive | `FLOAT_TO_TEXT` | |

## Text

Every operation on text is a primitive (architecture section 7). `lib/text.mo` holds
no functions, only tests that pin what reference section 14 says they do.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `"… #{x} …"`, interpolation | lowering | each part's text form, as `to_text` gives it, joined from the left with `Text.%concat` (architecture 4.3) | |
| `Text.%concat` | primitive | `TEXT_CONCAT` | lowering's own, for interpolation. It consumes its left side, so a text built from the left reuses its memory, as `append` does |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=`: equal when their bytes are | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | primitive | `TEXT_COMPARE`, by character code, then `eq` on the `Ordering` it gives | `bytecode.md` 4.4 compares only immediates |
| `byte_count` | primitive | `TEXT_BYTE_COUNT` | |
| `char_count` | primitive | `TEXT_CHAR_COUNT` | |
| `empty?` | primitive | `TEXT_IS_EMPTY` | |
| `chars` | primitive | `TEXT_CHARS` | |
| `bytes` | primitive | `TEXT_BYTES` | |
| `lines` | primitive | `TEXT_LINES` | |
| `words` | primitive | `TEXT_WORDS` | |
| `split(separator)` | primitive | `TEXT_SPLIT` | |
| `trim` | primitive | `TEXT_TRIM` | |
| `trim_start` | primitive | `TEXT_TRIM_START` | |
| `trim_end` | primitive | `TEXT_TRIM_END` | |
| `starts_with?(prefix)` | primitive | `TEXT_STARTS_WITH` | |
| `ends_with?(suffix)` | primitive | `TEXT_ENDS_WITH` | |
| `contains?(part)` | primitive | `TEXT_CONTAINS` | |
| `drop_prefix(prefix)` | primitive | `TEXT_DROP_PREFIX` | |
| `drop_suffix(suffix)` | primitive | `TEXT_DROP_SUFFIX` | |
| `replace(old, new)` | primitive | `TEXT_REPLACE` | |
| `repeat(n)` | primitive | `TEXT_REPEAT` | |
| `pad_left(width)` | primitive | `TEXT_PAD_LEFT` | |
| `pad_right(width)` | primitive | `TEXT_PAD_RIGHT` | |
| `ascii_lowercase` | primitive | `TEXT_ASCII_LOWERCASE` | |
| `ascii_uppercase` | primitive | `TEXT_ASCII_UPPERCASE` | |
| `to_int` | primitive | `TEXT_TO_INT` | |
| `to_decimal` | primitive | `TEXT_TO_DECIMAL` | |
| `to_float` | primitive | `TEXT_TO_FLOAT` | |
| `Text.from_chars(chars)` | primitive | `TEXT_FROM_CHARS` | |
| `to_text` | lowering | the text itself | |
| `Text.%quoted` | primitive | `TEXT_QUOTED` | lowering's own: inside another value's text form, a text is quoted and escaped as in code (reference 9.2) |
| `Text.%structural` | primitive | `TEXT_STRUCTURAL`: the VM's structural text form of a value of any type (`bytecode.md` 8) | lowering's own: `fail` gives both sides of a failed `==` or the value that didn't match a failed `is` (architecture 4.3), and those may have no `Text` ability, as `Bytes` and functions don't, or a `to_text` of their own that stops |

## Char

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a == b`, `a != b` | instruction | `eq` at `char`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | instruction | `lt` and `le` at `char`, by character code; `>` and `>=` swap the operands | |
| `code` | primitive | `CHAR_CODE` | |
| `ascii_digit?` | primitive | `CHAR_IS_ASCII_DIGIT` | |
| `ascii_letter?` | primitive | `CHAR_IS_ASCII_LETTER` | |
| `ascii_white_space?` | primitive | `CHAR_IS_ASCII_WHITE_SPACE` | |
| `ascii_lowercase` | primitive | `CHAR_ASCII_LOWERCASE` | |
| `ascii_uppercase` | primitive | `CHAR_ASCII_UPPERCASE` | |
| `to_text` | primitive | `CHAR_TO_TEXT` | |
| `Char.%quoted` | primitive | `CHAR_QUOTED` | lowering's own: inside another value's text form, a character is quoted and escaped as in code (reference 9.2) |

## Bytes

`Bytes` has no `Text` ability, so it has no `to_text` (reference 9.2).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | primitive | `BYTES_COMPARE`, byte by byte, then `eq` on the `Ordering` it gives | `bytecode.md` 4.4 compares only immediates |
| `size` | primitive | `BYTES_SIZE` | |
| `get(i)` | primitive | `BYTES_GET` | |
| `decode_utf8` | primitive | `BYTES_DECODE_UTF8` | |
| `slice(from, to)` | primitive | `BYTES_SLICE` | a copy: `Bytes` has no window onto another value's bytes, and the HTTP client's reader cuts at most a few times for each receive |
| `Bytes.join(parts)` | primitive | `BYTES_JOIN` | joining through a list of single bytes took 6.7 to 7.9 times the instructions in the HTTP prototype (`learnability/phase7/preflight/http-in-mo/`) |
| `to_hex` | primitive | `BYTES_TO_HEX` | lowercase RFC 4648 base16 encoding without per-byte allocation |
| `Bytes.from_hex(text)` | primitive | `BYTES_FROM_HEX` | accepts ASCII hex (upper or lower), rejects odd length or non-hex |
| `to_base64url` | primitive | `BYTES_TO_BASE64URL` | RFC 4648 §5 unpadded URL-safe base64 encoding |
| `Bytes.from_base64url(text)` | primitive | `BYTES_FROM_BASE64URL` | strict RFC 4648 §5 unpadded decoding: rejects invalid chars, `len % 4 == 1`, and non-zero trailing pad bits |
| `Bytes.empty` | primitive | `BYTES_EMPTY` | zero-byte constant, avoiding an empty list conversion |
| `Bytes.zeroed(count)` | primitive | `BYTES_ZEROED` | zero-filled buffer of length `count` (`0..MO_LIMIT_FILE_IO_BYTES`), allocated once |
| `Bytes.from_list(bytes)` | primitive | `BYTES_FROM_LIST` | packs a `List(Byte)` into contiguous `Bytes` in one pass |
| `to_list` | primitive | `BYTES_TO_LIST` | unpacks `Bytes` into `List(Byte)` in byte order |
| `Bytes.from_u16_be(n)` | primitive | `BYTES_FROM_U16_BE` | 2-byte big-endian unsigned integer (`0..65535`); out of range is a bug |
| `Bytes.from_u32_be(n)` | primitive | `BYTES_FROM_U32_BE` | 4-byte big-endian unsigned integer (`0..4294967295`); out of range is a bug |
| `Bytes.from_i64_be(n)` | primitive | `BYTES_FROM_I64_BE` | 8-byte big-endian two's-complement signed 64-bit integer |
| `u16_be_at(offset)` | primitive | `BYTES_U16_BE_AT` | reads 2 big-endian bytes as unsigned `Int` (`0..65535`), or `None` out of bounds |
| `u32_be_at(offset)` | primitive | `BYTES_U32_BE_AT` | reads 4 big-endian bytes as unsigned `Int` (`0..4294967295`), or `None` out of bounds |
| `i64_be_at(offset)` | primitive | `BYTES_I64_BE_AT` | reads 8 big-endian bytes as signed 64-bit `Int`, or `None` out of bounds |

## Unit

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=` | `bytecode.md` 4.4's `eq` doesn't take `unit` |
| `a < b`, `a <= b`, `a > b`, `a >= b` | lowering | `false` for `<` and `>`, and `true` for `<=` and `>=`, once both sides are computed: the one value is equal to itself | |
| `to_text` | lowering | `"Unit"` | |

## Tuples

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `pair.0`, `pair.1` | instruction | `field` | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | lowering | the generated `compare`, part by part from the left, then `eq` on its `Ordering` | |
| `to_text` | lowering | generated: `(1, true)` | |

## List(T)

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `[a, b, c]`, and `[]` | lowering | `List.%empty`, then `append` for each element, from the left | a primitive for the whole literal would take any number of parameters, which no signature in `prims.def` can say |
| `List.%empty` | primitive | `LIST_EMPTY` | lowering's own: `[]`, and the start of every list literal. With `append` on a list with one holder, a literal costs what one allocation would |
| `for x in items` | lowering | a join point that reads positions from 0 with `get` until it gives `None` | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | lowering | the generated `compare`, element by element, then `eq` on its `Ordering` | |
| `to_text` | lowering | generated: `[1, 2]` | |
| `size` | primitive | `LIST_SIZE` | |
| `empty?` | primitive | `LIST_IS_EMPTY` | |
| `get(i)` | primitive | `LIST_GET` | |
| `first` | primitive | `LIST_FIRST` | |
| `last` | primitive | `LIST_LAST` | |
| `append(x)` | primitive | `LIST_APPEND` | |
| `prepend(x)` | primitive | `LIST_PREPEND` | |
| `concat(other)` | primitive | `LIST_CONCAT` | |
| `reverse` | primitive | `LIST_REVERSE` | |
| `take(n)` | primitive | `LIST_TAKE` | |
| `drop(n)` | primitive | `LIST_DROP` | |
| `set(i, x)` | primitive | `LIST_SET` | |
| `remove_at(i)` | primitive | `LIST_REMOVE_AT` | |
| `map(f)` | Mo | `lib/list.mo` | takes a function |
| `filter(pred)` | Mo | `lib/list.mo` | takes a function |
| `filter_map(f)` | Mo | `lib/list.mo` | takes a function |
| `flat_map(f)` | Mo | `lib/list.mo` | takes a function |
| `reduce(start, f)` | Mo | `lib/list.mo` | takes a function |
| `find(pred)` | Mo | `lib/list.mo` | takes a function |
| `any?(pred)` | Mo | `lib/list.mo` | takes a function |
| `all?(pred)` | Mo | `lib/list.mo` | takes a function |
| `count(pred)` | Mo | `lib/list.mo` | takes a function |
| `enumerate` | primitive | `LIST_ENUMERATE` | a basic list operation; it allocates its list once |
| `zip(other)` | primitive | `LIST_ZIP` | a basic list operation; it allocates its list once |
| `sort_by(key)` | Mo | `lib/list.mo` | takes a function |
| `sort_by_desc(key)` | Mo | `lib/list.mo` | takes a function |
| `contains?(x)` | primitive | `LIST_CONTAINS` | needs only the VM's equality |
| `sort` | Mo | `lib/list.mo` | needs `compare`, which is Mo code for most types; a merge sort is n log n and stable (reference 13.3) |
| `min` | Mo | `lib/list.mo` | needs `compare`, as `sort` does |
| `max` | Mo | `lib/list.mo` | needs `compare`, as `sort` does |
| `unique` | primitive | `LIST_UNIQUE` | needs only the VM's equality and hashing |
| `to_set` | primitive | `LIST_TO_SET` | needs only the VM's equality and hashing |
| `group_by(key)` | Mo | `lib/list.mo` | takes a function |
| `sum` | primitive | `LIST_SUM` | Mo can't write the zero of a type parameter, since a literal's type is a concrete type (reference 3.3); the VM has the addition of all four `Number` types |
| `join(separator)` | primitive | `LIST_JOIN` | a text operation |
| `to_map` | primitive | `LIST_TO_MAP` | a later value wins, so it is not the map literal's primitive (architecture 4.3) |

## Map(K, V)

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `{k: v, …}`, and `{}` | lowering | every key and value, computed from the left; then `Map.%empty`, and `Map.%literal_insert` for each entry in order | as for a list literal: architecture 4.3's one primitive that builds the map from all its entries would take any number of parameters. Building it entry by entry, after computing them all, stops on the same literals at the same point |
| `Map.%empty` | primitive | `MAP_EMPTY` | lowering's own: `{}`, and the start of every map literal |
| `Map.%literal_insert` | primitive | `MAP_LITERAL_INSERT` | lowering's own: unlike `set`, it stops when its key is already there |
| `for (k, v) in m` | lowering | a loop over `entries`, as over a list | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=`: the same entries, in any order | |
| `to_text` | lowering | generated: `{"a": 1}` | |
| `size` | primitive | `MAP_SIZE` | |
| `empty?` | primitive | `MAP_IS_EMPTY` | |
| `get(key)` | primitive | `MAP_GET` | |
| `has?(key)` | primitive | `MAP_HAS` | |
| `set(key, value)` | primitive | `MAP_SET` | |
| `remove(key)` | primitive | `MAP_REMOVE` | |
| `update(key, default, f)` | Mo | `lib/map.mo` | takes a function |
| `keys` | primitive | `MAP_KEYS` | |
| `values` | primitive | `MAP_VALUES` | |
| `entries` | primitive | `MAP_ENTRIES` | |

## Set(T)

`lib/set.mo` tests these operations, `List.to_set`, insertion order, equality, hashing and text forms
against reference sections 3.8, 9.2 and 14. It has no functions; the operations stay primitives.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Set.empty` | primitive | `SET_EMPTY` | a constant, so its signature in `prims.def` is just its type |
| `for x in s` | lowering | a loop over `to_list`, as over a list | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=`: the same members, in any order | |
| `to_text` | lowering | generated: `[1, 2].to_set` | |
| `size` | primitive | `SET_SIZE` | |
| `empty?` | primitive | `SET_IS_EMPTY` | |
| `has?(x)` | primitive | `SET_HAS` | |
| `add(x)` | primitive | `SET_ADD` | |
| `remove(x)` | primitive | `SET_REMOVE` | |
| `union(other)` | primitive | `SET_UNION` | a basic set operation, which needs only the VM's equality and hashing |
| `intersection(other)` | primitive | `SET_INTERSECTION` | as for `union` |
| `difference(other)` | primitive | `SET_DIFFERENCE` | as for `union` |
| `to_list` | primitive | `SET_TO_LIST` | |

## Range

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a..b` | instruction | `make_range` | |
| `for i in r` | lowering | a join point that counts from `range_start` while below `range_end`, so the counter never overflows (architecture 4.3) | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=`: equal when their ends are | `bytecode.md` 4.4's `eq` doesn't take `range` |
| `to_text` | lowering | `0..3`: the ends' `to_text`, from `range_start` and `range_end`, joined with `Text.%concat` | |
| `to_list` | primitive | `RANGE_TO_LIST` | a conversion; it allocates its list once |
| `contains?(n)` | primitive | `RANGE_CONTAINS` | Mo code can't read a range's ends |

## Time

Every operation on times and durations is an instruction or a primitive: Mo code can
neither build a `Time` from its parts nor read them. `lib/time.mo` holds no functions, only
tests that pin what reference sections 3.9 and 14 say they do, with `Clock.fixed`'s.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `t + d`, `t - d`, with a `Duration` | instruction | `add_time` and `sub_time` | |
| `t - u`, two times | instruction | `diff_time`, which gives a `Duration` | |
| `a == b`, `a != b` | instruction | `eq` at `time`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | instruction | `lt` and `le` at `time`; `>` and `>=` swap the operands | |
| `checked_add(d)` | primitive | `TIME_CHECKED_ADD` | |
| `to_text` | primitive | `TIME_TO_TEXT` | |
| `Time.parse(text)` | primitive | `TIME_PARSE` | |

## Duration

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a + b`, `a - b` | instruction | `add` and `sub` at `duration` | |
| `d * n`, with an `Int` | instruction | `scale_duration` | |
| `-d` | instruction | `neg` at `duration` | |
| `a == b`, `a != b` | instruction | `eq` at `duration`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | instruction | `lt` and `le` at `duration`; `>` and `>=` swap the operands | |
| `in_milliseconds` | primitive | `DURATION_IN_MILLISECONDS` | |
| `in_seconds` | primitive | `DURATION_IN_SECONDS` | |
| `to_text` | primitive | `DURATION_TO_TEXT` | |

## Records and choice types

This includes the library's own choice types, `Option`, `Result`, `Ordering`
and `FileError`, whose cases are always in scope (reference 10.5).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Point(x: 1, y: 2)`, `Some(3)` | instruction | `construct` or `make_case` | |
| `point.x`, and `case` | instruction | `field`; `switch` and `case_field` | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=`; a choice type whose cases have no fields, such as `Ordering`, is an immediate, compared with `eq` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | lowering | the type's own `compare`, or the generated one: fields in declaration order, cases in declaration order; then `eq` on its `Ordering` | |
| `to_text` | lowering | the type's own `to_text`, or the generated one: `Point(x: 1.5, y: 2)`, `Some(3)` | |

## Option(T)

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `x or default` | lowering | a `case`: `Some(v)` gives `v`, and `None` computes `default` (architecture 4.3) | `default` is computed only for `None` (reference 13.1), which no function can do |
| `try x` | lowering | a `case` that returns `None` from the function | |
| `or_fail(e)` | Mo | `lib/option.mo` | Mo expresses it as one `case` |
| `map(f)` | Mo | `lib/option.mo` | takes a function |

## Result(T, E)

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `try x` | lowering | a `case` that returns the `Error` from the function, wrapped in its failure type where reference 8.3 says (architecture 4.3) | |
| `map(f)` | Mo | `lib/result.mo` | takes a function |
| `map_error(f)` | Mo | `lib/result.mo` | takes a function |

## Args

Capabilities have no abilities (reference 11.4): no `==`, no order and no text
form. Their operations are host operations, and the fakes are made only in
spec code (reference 12.4).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `list` | host | `ARGS_LIST` | |
| `Args.of(list)` | host | `ARGS_OF` | a fake, for spec code only (reference 12.4) |

## Env

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `get(name)` | host | `ENV_GET` | |
| `Env.of(map)` | host | `ENV_OF` | a fake, for spec code only (reference 12.4) |

## Console

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `print(text)` | host | `CONSOLE_PRINT` | |
| `error(text)` | host | `CONSOLE_ERROR` | |
| `read_line` | host | `CONSOLE_READ_LINE` | |
| `printed` | host | `CONSOLE_PRINTED` | spec code only (reference 12.4) |
| `printed_errors` | host | `CONSOLE_PRINTED_ERRORS` | spec code only (reference 12.4) |
| `Console.fake()` | host | `CONSOLE_FAKE` | a fake, for spec code only (reference 12.4) |
| `Console.fake_input(lines)` | host | `CONSOLE_FAKE_INPUT` | a fake, for spec code only (reference 12.4) |

## Files

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `read_text(path)` | host | `FILES_READ_TEXT` | |
| `read_bytes(path)` | host | `FILES_READ_BYTES` | |
| `write_text(path, text)` | host | `FILES_WRITE_TEXT` | |
| `append_text(path, text)` | host | `FILES_APPEND_TEXT` | |
| `remove(path)` | host | `FILES_REMOVE` | |
| `make_folder(folder)` | host | `FILES_MAKE_FOLDER` | |
| `list(folder)` | host | `FILES_LIST` | |
| `entries(folder)` | host | `FILES_ENTRIES` | |
| `walk(folder)` | Mo | `lib/files.mo` | `ReadFiles`'s `walk` on `read_only` |
| `exists?(path)` | host | `FILES_EXISTS` | |
| `within(folder)` | host | `FILES_WITHIN` | |
| `read_only` | host | `FILES_READ_ONLY` | |
| `write_bytes(path, data)` | host | `FILES_WRITE_BYTES` | whole-file binary write with strict no-symlink confinement and parent-dir sync on create |
| `rename(from, to)` | host | `FILES_RENAME` | atomic `renameat` within the capability's folder with strict confinement and parent-dir sync |
| `open(path)` | host | `FILES_OPEN` | opens or creates a confined `Files.File` handle for random-access binary I/O |
| `read_at(offset, count)` | host | `FILES_READ_AT` | positional read on `Files.File` (`pread` loop on real files), capped at `MO_LIMIT_FILE_IO_BYTES` |
| `write_at(offset, data)` | host | `FILES_WRITE_AT` | positional write on `Files.File` (`pwrite` loop on real files), zero-extending past EOF |
| `size` | host | `FILES_SIZE` | current byte length of an open `Files.File` |
| `truncate(size)` | host | `FILES_TRUNCATE` | resizes an open `Files.File` (truncating or zero-extending) |
| `sync` | host | `FILES_SYNC` | flushes `Files.File` data to durable storage (`F_FULLFSYNC` with `fsync` fallback on macOS, `fsync` on Linux) |
| `Files.in_memory(files)` | host | `FILES_IN_MEMORY` | a fake, for spec code only (reference 12.4); stores raw bytes so binary and `Files.File` ops match real files |

## ReadFiles

`ReadFiles` is a capability type of its own (`bytecode.md` 2.1), so its
operations have lines of their own in `hostops.def`. The host runs them with
the same code as `Files`'s, on a handle marked read-only (`bytecode.md` 7).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `read_text(path)` | host | `READFILES_READ_TEXT` | |
| `read_bytes(path)` | host | `READFILES_READ_BYTES` | |
| `list(folder)` | host | `READFILES_LIST` | |
| `entries(folder)` | host | `READFILES_ENTRIES` | |
| `walk(folder)` | Mo | `lib/read-files.mo` | written over `entries` one folder at a time, entering folders by path rather than with `within`, so it holds no folder open (D232) |
| `exists?(path)` | host | `READFILES_EXISTS` | |
| `within(folder)` | host | `READFILES_WITHIN` | |

## FileError

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `NotFound(path: p)`, and the other cases | instruction | `make_case`; `switch` and `case_field` | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=` | |
| `to_text` | Mo | `lib/file-error.mo` | a type's own `to_text` is an ordinary function in its module (reference 9.2), and Mo expresses this one as one `case` |

## Entry and EntryKind

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Entry(name: n, kind: k)`, and `EntryKind`'s cases | instruction | `construct` and `make_case`; `field` and `switch` | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=` | |

## Network

The real network is C in `src/host/real-network.c` (with TLS in
`src/host/tls.c`), and the fake is C in `src/host/fake-network.c`.
`src/host/network.c` holds the checks both share.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `connect(host, port, within)` | host | `NETWORK_CONNECT` | |
| `connect_secure(host, port, within)` | host | `NETWORK_CONNECT_SECURE` | |
| `allow_hosts(hosts)` | host | `NETWORK_ALLOW_HOSTS` | |
| `Network.fake(peers)` | host | `NETWORK_FAKE` | a fake, for spec code only (reference 12.4) |

## Connection

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `send(bytes)` | host | `CONNECTION_SEND` | |
| `receive(most)` | host | `CONNECTION_RECEIVE` | |

## NetworkError

`NetworkError` has no `to_text` of its own, so its text form is the one
generated for every type with `Text` (the first table).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Network.Refused(host: h, port: p)`, and the other cases | instruction | `make_case`; `switch` and `case_field` | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=` | |

## Sqlite

Real databases use `src/host/sqlite.c` with SQLite inside the program's
starting folder; the fake keeps each database as serialized bytes in memory.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `open(path)` | host | `SQLITE_OPEN` | |
| `Sqlite.fake()` | host | `SQLITE_FAKE` | a fake, for spec code only (reference 12.4) |
| `Sqlite.in_memory()` | lowering | `src/lower/applications.c` | spec-only alias for `Sqlite.fake()`, matching `Files.in_memory` |
| `run(statement, values)` | host | `SQLITE_RUN` | |
| `query(statement, values)` | host | `SQLITE_QUERY` | |
| `execute_raw(statement, values)` | host | `SQLITE_EXECUTE_RAW` | returns `(changes, last_insert_rowid)` from `sqlite3_total_changes64` delta and `sqlite3_last_insert_rowid` |
| `execute(statement, values)` | Mo | `lib/sqlite.mo` | wraps `SQLITE_EXECUTE_RAW` in `Sqlite.ExecResult` |
| `Sqlite.transaction(db, ctx, work, on_sql)` | Mo | `lib/sqlite.mo` | runs `BEGIN IMMEDIATE`, commits on `Ok`, rolls back on `Error` |
| `Sqlite.decode(rows)` | lowering | `src/lower/abilities.c`, `lib/sqlite.mo` | compiler-derived struct row decoder with column-name matching and cell validation |
| `Sqlite.decode_one(rows)` | lowering | `src/lower/abilities.c`, `lib/sqlite.mo` | single-row variant returning `ExpectedOneRow` when `rows.rows.size != 1` |
| `Sqlite.decode_optional(rows)` | lowering | `src/lower/abilities.c`, `lib/sqlite.mo` | optional-row variant (`0` -> `None`, `1` -> `Some`, `>1` -> `ExpectedOneRow`) |
| `SqliteDecodeError.to_text` | Mo | `lib/sqlite.mo` | |

## Command

`Command` (`use Command`) runs confined child processes in `src/host/command.c`
(`mo_command_confine` in `src/host/confine.c` applies Landlock filesystem scoping
and seccomp network/mount/signal filtering on Linux) and decodes structured
results in `lib/command.mo`.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `run_raw(program, args, within)` | host | `COMMAND_RUN_RAW` | spawns `program` inside the capability's folder with monotonic deadline and 4 MiB pipe cap, or looks up a `Command.fake` exchange |
| `within(folder)` | host | `COMMAND_WITHIN` | narrows the working folder with `mo_host_resolve` (`RESOLVE_BENEATH`), poisoning escapes |
| `allow_programs(programs)` | host | `COMMAND_ALLOW_PROGRAMS` | intersects allowed program basenames so authority only narrows |
| `Command.fake(replies)` | host | `COMMAND_FAKE` | a fake, for spec code only (reference 12.4) |
| `run(program, args, within)` | Mo | `lib/command.mo` | decodes UTF-8 `stdout`/`stderr` into `Output` and maps error tags into `CommandError` |
| `Command.spec(program, args)` | Mo | `lib/command.mo` | starts a `Spec` at the capability's root folder |
| `in_folder(folder)` | Mo | `lib/command.mo` | narrows a `Spec` to a relative subfolder |
| `Command.output(status, stdout, stderr)` | Mo | `lib/command.mo` | builds an `Output` |
| `Command.ok_output(stdout)` | Mo | `lib/command.mo` | builds a zero-exit `Output` with empty `stderr` |
| `ok?` | Mo | `lib/command.mo` | whether `status == 0` |
| `Command.replies(exchanges)` | Mo | `lib/command.mo` | encodes `(Spec, Output)` pairs into the length-prefixed `Map(Bytes, Bytes)` table `Command.fake` reads |

## Json

`Json` is a module of the library, written in Mo (D232). The VM needs nothing
new for it: it parses and prints with text and list primitives.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Json.parse(text)` | Mo | `lib/json.mo` | a parser over `text.chars`, with the nesting limit of reference 14; a string with no escape and no control character is read whole, from `text.split` at the quotes |
| `json.to_text`, `json.pretty` | Mo | `lib/json.mo` | |
| `json.get(key)`, `json.at(index)`, `json.path(steps)` | Mo | `lib/json.mo` | a path's steps are `steps.split(".")`, each a key in an object and an index in an array; the empty path is the value itself |
| `text_at`, `int_at`, `bool_at` and `list_at` on a `Json` | Mo | `lib/json.mo` | `path`, then the one kind of value each gives |
| `Json.int(n)`, `Json.decimal(d)`, `Json.float(f)` | Mo | `lib/json.mo` | |
| `to_int`, `to_decimal`, `to_float` and `to_text` on a `JsonNumber` | Mo | `lib/json.mo` | the parser has already checked JSON's own number syntax; `to_float` is `Text.to_float` on the number's text, and `to_int` and `to_decimal` first write it without its exponent, so `1.5e1` is the `Int` 15 |
| `value.to_json`, `T.to_json(value)` | lowering | `src/lower/abilities.c` | derived over records, choice cases (`snake_case` tags), tuples and collections, or calls the declaring module's `to_json` |
| `Json.encode(value)`, `Json.encode_pretty(value)` | Mo | `lib/json.mo` | `value.to_json` followed by `to_text` or `pretty` |
| `Json.decode[T](input)`, `Json.decode_strict[T](input)` | lowering | `src/lower/abilities.c` | `Json.parse` (after `Bytes.decode_utf8` for `Bytes`), then derived or custom `from_json` with dotted error paths |
| `Json.from_json[T](json)`, `Json.from_json_strict[T](json)` | lowering | `src/lower/abilities.c` | derived field and case decoding, or the declaring module's `from_json` |
| `Json.schema[T]()` | lowering | `src/lower/abilities.c` | structural JSON Schema object for `T`, with `$ref` on recursive nominal types |

## Http

`Http` is a module of the library, written in Mo on `Bytes` and the `Network` capability. The VM
needs nothing new for it. The only C that touches a response's bytes is `receive`, which copies
them and reads none.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Http.get(url)`, `Http.post(url)`, `Http.request(method, url)` | Mo | `lib/http.mo` | |
| `header`, `query`, `form`, `json`, `text`, `within` and `max_body` on a `Request` | Mo | `lib/http.mo` | each gives a changed copy and checks nothing; `send` reports a bad URL, method or header |
| `request.send(network)` | Mo | `lib/http.mo` | one `connect` or `connect_secure`, one `send`, then `receive` of up to 1 MiB at a time until the response is whole; after each receive the reader reads the headers again and reads a chunked body on from the chunk it stopped at; headers are capped at 64 KiB and the body at `request.body_limit` (16 MiB by default) |
| `ok?`, `header(name)`, `headers_named(name)`, `text` and `json` on a `Response` | Mo | `lib/http.mo` | `json` is `Json.parse` on the body |
| `Http.reply(status, body)`, `Http.replies(exchanges)` | Mo | `lib/http.mo` | for tests: `replies` writes each request with the writer `send` uses, under its host and port |

## Api

`Api` is a module of the library, written in Mo (`lib/api.mo`), supplying the
`Request` and `Response` types and routing, query, path, CORS, SSE and OpenAPI
helpers that lowered `api` servers (`src/lower/apis.c`) and handlers call.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Api.request(method, target)`, `Api.get(target)`, `Api.post(target)`, `Api.put(target)`, `Api.patch(target)`, `Api.delete(target)` | Mo | `lib/api.mo` | splits `target` at `?` into normalized path and percent-decoded query list |
| `with_header`, `add_header`, `with_bearer`, `with_query`, `with_json`, `with_text` and `with_bytes` on an `Api.Request` | Mo | `lib/api.mo` | immutable request builders for `ApiTest.send` and middleware helpers |
| `header`, `headers_named`, `bearer_token`, `query_param` and `text` on an `Api.Request` | Mo | `lib/api.mo` | case-insensitive single and multi-header lookup and Bearer extraction |
| `Api.response(status, headers, body)`, `Api.json_response(status, value)`, `Api.text_response(status, body)`, `Api.html_response(status, html)`, `Api.no_content()`, `Api.redirect(location)`, `Api.redirect_see_other(location)`, `Api.redirect_with(status, location)`, `Api.sse_response(events)`, `Api.serve_static_fake(req, prefix, dir, files)` | Mo | `lib/api.mo` | response constructors, 3xx redirects, SSE and `ApiTest.send` static asset serving |
| `ok?`, `with_header`, `add_header`, `header`, `headers_named`, `text` and `json` on an `Api.Response` | Mo | `lib/api.mo` | response header appending (including multiple `Set-Cookie` headers) and inspection |
| `Api.encode_job_payload(payload)`, `Api.parse_job_payload(raw)`, `Api.unknown_job_error(name)`, `Api.sqlite_job_error(err)` | Mo | `lib/api.mo` | JSON payload encoding/decoding and error formatting for lowered `%api.job` and `ApiTest.run_job` dispatch |
| `ApiTest.send(module, request, ...)`, `ApiTest.run_job(module, name, payload, ...)` | lowering | `src/lower/apis.c` | spec-only direct calls to `Module.%api.dispatch` and `Module.%api.job` with real or synthesized fake capabilities and actors |

## Actor

`actor Name ... end` declares a typed in-process BEAM actor capability (reference 11.7). Its
handlers compile to a service descriptor (`_MO_ACTOR`) without durable storage or tokens, and calls
dispatch over `MO_SIM_SEND` against either a deterministic `mo_simulation` in tests or a
mutex-serialized `mo_live_actor` in `mo serve` and `mo run`.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Actor.start(Type)`, `Actor.start_at(Type, time)` | lowering | `src/lower/applications.c` | spec-only deterministic actor handles backed by `MO_SIM_START` |
| `Actor.send(handle, Type.call, ...)` | lowering | `src/lower/applications.c` | explicit `MO_SIM_SEND` returning `Result(Reply, SimulationError)` with state projected out |
| `Actor.advance(handle, duration)`, `Actor.crash(handle)`, `Actor.restart(handle)` | lowering | `src/lower/applications.c` | spec-only fake clock and supervisor controls backed by `MO_SIM_ADVANCE`, `MO_SIM_CRASH` and `MO_SIM_RESTART` |

## Clock

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `now` | host | `CLOCK_NOW` | |
| `sleep(duration)` | host | `CLOCK_SLEEP` | `nanosleep` on a real clock, instant advance on `Clock.fixed` |
| `Clock.fixed(time)` | host | `CLOCK_FIXED` | a fake, for spec code only (reference 12.4) |

## Random

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `int(low, high)` | host | `RANDOM_INT` | |
| `Random.seeded(seed)` | host | `RANDOM_SEEDED` | a fake, for spec code only (reference 12.4) |

## SecureRandom

Real cryptographic randomness wraps OpenSSL 3 `RAND_bytes` in `src/host/random.c`
(failing closed on OS entropy errors). `SecureRandom.seeded` produces a deterministic
SHA-256 counter stream for tests, examples, and promises.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `bytes(count)` | host | `SECURERANDOM_BYTES` | CSPRNG byte stream (`0..65536` bytes per call; real uses `RAND_bytes`) |
| `SecureRandom.seeded(seed)` | host | `SECURERANDOM_SEEDED` | deterministic fake CSPRNG, for spec code only (reference 12.4) |

## Crypto

`Crypto` (`use Crypto`) wraps OpenSSL 3 `libcrypto` in `src/prim/crypto.c` (zero hand-rolled
cryptographic algorithms) and exposes typed `CryptoError` wrappers in `lib/crypto.mo`.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Crypto.sha256(data)` | primitive | `CRYPTO_SHA256` | OpenSSL `EVP_Digest` + `EVP_sha256()` |
| `Crypto.hmac_sha256(key, data)` | primitive | `CRYPTO_HMAC_SHA256` | OpenSSL `EVP_Q_mac` HMAC-SHA256 |
| `Crypto.constant_time_eq?(a, b)` | primitive | `CRYPTO_CONSTANT_TIME_EQ` | OpenSSL `CRYPTO_memcmp` without early exit on equal-length inputs |
| `Crypto.seal_raw(key, nonce, plaintext, aad)` | primitive | `CRYPTO_SEAL_RAW` | OpenSSL `EVP_aes_256_gcm()` returning ciphertext plus 16-byte tag or error tag |
| `Crypto.open_raw(key, nonce, ciphertext, aad)` | primitive | `CRYPTO_OPEN_RAW` | OpenSSL `EVP_aes_256_gcm()` verifying the 16-byte tag before releasing plaintext |
| `Crypto.hash_password_raw(password, salt)` | primitive | `CRYPTO_HASH_PASSWORD_RAW` | OpenSSL `EVP_PBE_scrypt` (`ln=15,r=8,p=1`) producing a PHC string |
| `Crypto.verify_password(password, encoded)` | primitive | `CRYPTO_VERIFY_PASSWORD` | strict PHC scrypt parser with DoS bounds + `EVP_PBE_scrypt` + `CRYPTO_memcmp` |
| `Crypto.seal(key, nonce, plaintext, aad)` | Mo | `lib/crypto.mo` | wraps `seal_raw` into `Result(Bytes, CryptoError)` |
| `Crypto.open(key, nonce, ciphertext, aad)` | Mo | `lib/crypto.mo` | wraps `open_raw` into `Result(Bytes, CryptoError)` |
| `Crypto.hash_password(password, salt)` | Mo | `lib/crypto.mo` | wraps `hash_password_raw` into `Result(Text, CryptoError)` |

## Date

`Date` (`use Date`) is a pure-Mo calendar date module in `lib/date.mo` providing an opaque
validated Gregorian `Date` (years 1..9999) with custom `Text` and `Json` (`"YYYY-MM-DD"`).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Date.from_ymd(year, month, day)` | Mo | `lib/date.mo` | validates Gregorian year, month, and leap-year day bounds |
| `Date.parse_iso(text)` | Mo | `lib/date.mo` | strict `"YYYY-MM-DD"` ASCII parser |
| `Date.from_time_utc(time)` | Mo | `lib/date.mo` | extracts UTC Gregorian calendar date from `Time` |
| `to_iso`, `to_text`, `to_json`, `from_json` | Mo | `lib/date.mo` | `"YYYY-MM-DD"` formatting and custom `Text` / `Json` abilities |
| `year`, `month`, `day`, `year_month`, `weekday`, `days_in_month` | Mo | `lib/date.mo` | component accessors and exact Gregorian weekday / month-length helpers |
| `add_days`, `add_months`, `days_between`, `first_of_month`, `end_of_month`, `short_label` | Mo | `lib/date.mo` | exact integer calendar arithmetic with safe month-end clamping and formatting |

## Unicode16

`lib/unicode16.mo` supplies the module and tests its primitive operations against
the reference's section 14 and the Unicode policy below.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Unicode16.graphemes(text)` | primitive | `UNICODE16_GRAPHEMES` | |
| `Unicode16.display_width(text)` | primitive | `UNICODE16_DISPLAY_WIDTH` | |
| `Unicode16.normalize_nfc(text)` | primitive | `UNICODE16_NORMALIZE_NFC` | |
| `Unicode16.lowercase(text)` | primitive | `UNICODE16_LOWERCASE` | |
| `Unicode16.uppercase(text)` | primitive | `UNICODE16_UPPERCASE` | |
| `Unicode16.letter?(c)` | primitive | `UNICODE16_LETTER` | Char → Bool; General_Category L (`Lu`, `Ll`, `Lt`, `Lm`, `Lo`) |
| `Unicode16.digit?(c)` | primitive | `UNICODE16_DIGIT` | Char → Bool; General_Category `Nd` |

These trusted primitives use Unicode **16.0.0**, independent of the host's
locale or Unicode library. `letter?` and `digit?` classify one code point,
without normalization. Neither includes `Nl`, `No` or marks; `letter?` is not
the derived Alphabetic property. Their flags in `src/prim/unicode16-data.h`
come from the pinned `UnicodeData.txt`, including its First/Last ranges.
`graphemes` returns the default extended grapheme
clusters of UAX #29 revision 45, without tailoring or changing their bytes.
`normalize_nfc` implements canonical decomposition, ordering and composition,
not compatibility normalization. Case conversion uses full mappings from
`UnicodeData` and `SpecialCasing`, including language-insensitive final sigma
on the original text; Turkish, Azeri and Lithuanian rules are excluded. It
does not normalize its result.

Final sigma skips `Case_Ignorable` scalars before testing `Cased`, following
ICU, CPython and Rust. This also skips the 267 Unicode 16.0 scalars with both
properties; they do not establish or terminate the cased context. In
particular, `lowercase("ʰΣ")` is `"ʰσ"`, not `"ʰς"`. This resolves the
overlap in the literal wording of Unicode Table 3-17.

`display_width` is a deterministic column estimate, **not a terminal or font
measurement**. Split the original text into the same extended graphemes and
sum their widths. A grapheme's width is the maximum of its scalar widths:

- General categories `Mn`, `Mc`, `Me` and `Default_Ignorable_Code_Point` have
  width 0; this takes precedence over East Asian Width.
- C0 and C1 controls and DEL have width 0, including tabs and line endings.
  CRLF is one grapheme of width 0. There are no tab stops, cursor movements
  or line resets in this calculation (D238).
- East Asian Width `W` and `F` have width 2; all other scalars have width 1.
  Ambiguous width stays 1.
- An unpaired regional indicator has width 1, despite its
  `Emoji_Presentation` property. A regional-indicator pair gives the
  grapheme width 2 (D238).
- Emoji presentation gives width 2 to the whole grapheme: a scalar with
  `Emoji_Presentation` other than a regional indicator, a registered base
  plus emoji selector U+FE0F, or a keycap `#`, `*`, `0`–`9` plus optional
  U+FE0F and U+20E3. Regional-indicator pairs,
  modifier, tag and ZWJ sequences with emoji-presenting components therefore
  have width 2, not the sum of their components. A text selector does not
  override a scalar's `W`/`F` width.

An empty text or a grapheme consisting only of zero-width scalars has width
0. This policy deliberately gives width 1 to multi-letter Indic conjuncts
and width 2 to conjoining wide Hangul. Unlike Rust's `unicode-width` 0.2,
Mo takes a maximum per grapheme instead of summing scalar widths with
ligature rules, and treats spacing combining marks (`Mc`) as zero. For
example, `का` has width 1 in Mo and 2 there. Single scalars and complex
clusters can therefore differ; neither policy specifies a font's rendering.
Mo's policy is versioned with `Unicode16`, not the environment.

## Simulation

The fake capability is created only in spec code. Helpers that receive it
may use the remaining operations. Selectors are compile-time syntax, not
function values. `send` returns `Result(R, SimulationError)` for the complete
declared tool result R; none of these operations defines a transport schema.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Simulation.start(module, at)` | instruction | `SIM_START` | spec-only fake store and real service loop |
| `Simulation.send(handle, module.tool, args)` | instruction | `SIM_SEND` | selected tool signature and cross-heap saved codec |
| `Simulation.advance(handle, by)` | instruction | `SIM_ADVANCE` | every due deadline on the fake timeline |
| `Simulation.crash(handle)` | instruction | `SIM_CRASH` | discard loop, retain open store and clock |
| `Simulation.restart(handle)` | instruction | `SIM_RESTART` | real recovery and fresh supervisor |

## Functions

Functions have no abilities (reference 9.2).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `f(a, b)`, calling a function value | instruction | `call_closure`, or `tail_call_closure` in tail position | |

## Arvo.Cookie

`Arvo.Cookie` (`use Arvo.Cookie`) parses `Cookie` headers and appends injection-safe `Set-Cookie` headers in pure Mo.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `default_options()`, `session_options(secure?, max_age)`, `valid_name?(name)`, `valid_value?(value)` | Mo | `lib/arvo/cookie.mo` | RFC 6265 token and value validation |
| `parse(req)`, `get(req, name)`, `set(res, name, value, opts)`, `clear(res, name, path, secure?)` | Mo | `lib/arvo/cookie.mo` | multi-header parsing and `Set-Cookie` builders |

## Arvo.Session

`Arvo.Session` (`use Arvo.Session`) implements AES-256-GCM encrypted cookies backed by revocable SQLite `arvo_sessions` rows storing SHA-256 token digests.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `default_config(secure?)`, `schema_sql()`, `init_schema(db)` | Mo | `lib/arvo/session.mo` | session configuration and `arvo_sessions` DDL |
| `create(db, rng, clock, key, member_id, cfg)`, `load(req, db, clock, key, cfg)`, `rotate(db, rng, clock, key, old_id, member_id, cfg)` | Mo | `lib/arvo/session.mo` | AEAD cookie issuance, verification and atomic rotation |
| `revoke(db, id)`, `revoke_all_for_member(db, member_id)`, `prune_expired(db, clock)`, `attach_cookie(res, val, cfg)`, `clear_cookie(res, cfg)` | Mo | `lib/arvo/session.mo` | revocation, expiry pruning and cookie attachment |

## Arvo.Csrf

`Arvo.Csrf` (`use Arvo.Csrf`) verifies double-submit + session-bound `XSRF-TOKEN` / `X-XSRF-TOKEN` tokens in constant time.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `safe_method?(req)`, `verify(req, session)`, `verify_token(token, session)` | Mo | `lib/arvo/csrf.mo` | constant-time CSRF check compatible with `@inertiajs/react` |
| `attach_cookie(res, session, secure?)`, `clear_cookie(res, secure?)` | Mo | `lib/arvo/csrf.mo` | readable `XSRF-TOKEN` cookie helpers |

## Arvo.Flash

`Arvo.Flash` (`use Arvo.Flash`) carries AEAD-sealed one-hop notices, alerts and validation error bags across `303 See Other` redirects.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `empty()`, `empty?(data)`, `with_notice(msg)`, `with_alert(msg)`, `with_errors(errs)` | Mo | `lib/arvo/flash.mo` | `FlashData` constructors and predicates |
| `attach(res, rng, key, data, secure?)`, `take(req, key)`, `clear_if_present(res, had_cookie?, secure?)` | Mo | `lib/arvo/flash.mo` | AES-256-GCM sealing, fail-closed opening and eviction |

## Arvo.Form

`Arvo.Form` (`use Arvo.Form`) parses bounded `application/x-www-form-urlencoded` and binary-safe `multipart/form-data` bodies.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `parse_urlencoded(req)`, `parse_multipart(req, max_bytes)`, `field(fields, name)`, `file(form, field_name)` | Mo | `lib/arvo/form.mo` | bounded form and multipart file upload parsers |

## Arvo.Inertia

`Arvo.Inertia` (`use Arvo.Inertia`) implements the Inertia v2 server protocol with Vite dev HMR and production `manifest.json` asset resolution.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `dev_config(title, dev_url, entry_js, head_extra)`, `version_from_manifest(bytes)`, `config_from_manifest(title, bytes, entry_key, head_extra)` | Mo | `lib/arvo/inertia.mo` | Vite dev and production manifest configuration |
| `request?(req)`, `escape_html(raw)`, `page_url(req)`, `version_mismatch?(req, cfg)`, `check_version(req, cfg)` | Mo | `lib/arvo/inertia.mo` | Inertia header detection, HTML attribute escaping and `409` version checks |
| `render(req, cfg, component, props, shared)`, `render_with_status(req, cfg, status, component, props, shared)`, `redirect(target)`, `location(url)`, `validation_error(req, rng, key, fallback_url, errors, secure?)` | Mo | `lib/arvo/inertia.mo` | partial-reload prop filtering, HTML/JSON responses and same-origin `303` error redirects |

## Arvo.Changeset

`Arvo.Changeset` (`use Arvo.Changeset`) accumulates typed field validation errors and formats Inertia error maps.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `new(data)`, `add_error(cs, field, msg)`, `check(cs, field, ok?, msg)`, `valid?(cs)`, `finish(cs)` | Mo | `lib/arvo/changeset.mo` | pure validation accumulator |
| `require_non_empty`, `require_length`, `require_positive_int`, `require_non_negative_int`, `require_email`, `require_one_of` | Mo | `lib/arvo/changeset.mo` | built-in field validators |
| `first_errors(errs)`, `errors_json(cs)`, `error_map_json(errs)`, `inertia_errors_json(cs)`, `inertia_error_map_json(errs)` | Mo | `lib/arvo/changeset.mo` | full and flat `useForm().errors` JSON encoders |

## Arvo.Migrations

`Arvo.Migrations` (`use Arvo.Migrations`) runs ordered, SHA-256 checksum-verified SQL migrations tracked in `_arvo_migrations`.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `split_statements(sql)`, `run(db, clock, migrations)`, `applied_versions(db)` | Mo | `lib/arvo/migrations.mo` | trigger-aware SQL splitter and transactional migration runner |

## Arvo.Audit

`Arvo.Audit` (`use Arvo.Audit`) records ULID-ordered `arvo_events` audit entries and checks optimistic concurrency updates.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `schema_sql()`, `init_schema(db)`, `generate_id(rng, clock)`, `ensure_updated(exec)` | Mo | `lib/arvo/audit.mo` | `arvo_events` DDL, Crockford Base32 ULIDs and `version` checks |
| `record(db, rng, clock, actor_name, action, entity_table, entity_id, before, after, reason, undo)`, `list_recent(db, limit)`, `get_by_id(db, id)` | Mo | `lib/arvo/audit.mo` | audit event insertion and `Sqlite.decode[AuditEntry]` queries |

## Arvo.Auth

`Arvo.Auth` (`use Arvo.Auth`) provides email allowlist checks, CSPRNG-salted password hashing and dual-cookie session/CSRF helpers.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `normalize_email(email)`, `email_allowed?(email, allowlist)`, `hash_password(rng, password)`, `verify_password(password, encoded)` | Mo | `lib/arvo/auth.mo` | normalized allowlist and scrypt password wrappers |
| `login_response(res, session, cookie_val, cfg)`, `logout_response(res, cfg)`, `require_csrf(req, session)` | Mo | `lib/arvo/auth.mo` | attaches/clears both `arvo_session` and `XSRF-TOKEN` cookies |

## Arvo.WebTest

`Arvo.WebTest` (`use Arvo.WebTest`) wraps `Api.Request` and `Api.Response` in a stateful cookie, CSRF and Inertia v2 test client.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `new()`, `with_inertia(client, version)`, `without_inertia(client)`, `with_header(client, name, value)` | Mo | `lib/arvo/web-test.mo` | immutable test client builders |
| `prepare(client, req)`, `prepare_without_csrf(client, req)`, `absorb(client, res)`, `cookie(client, name)`, `redirect_location(res)`, `inertia_page(res)` | Mo | `lib/arvo/web-test.mo` | cookie jar, CSRF injection and Inertia JSON/HTML `data-page` decoding |

## Arvo.Jobs

`Arvo.Jobs` (`use Arvo.Jobs`) implements a durable SQLite `arvo_jobs` background job queue with transactional enqueueing, atomic `BEGIN IMMEDIATE` claiming, exponential backoff retries and orphaned-job recovery.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `schema_sql()`, `init_schema(db)`, `backoff_seconds(attempts)` | Mo | `lib/arvo/jobs.mo` | `arvo_jobs` DDL + ready index and bounded `15 * 4^(attempts - 1)` backoff (`15..3600` seconds) |
| `enqueue(db, rng, clock, job_name, payload)`, `enqueue_at(db, rng, clock, queue, job_name, payload, run_at, max_attempts)` | Mo | `lib/arvo/jobs.mo` | transaction-composable ULID job insertion without opening a nested SQLite transaction |
| `claim_next(db, clock, job_names)`, `complete(db, clock, job_id)`, `fail_or_retry(db, clock, job_id, attempts, max_attempts, error_msg)`, `requeue_stale_running(db, clock, stale_seconds)`, `list_recent(db, limit)` | Mo | `lib/arvo/jobs.mo` | atomic FIFO due-job claiming, completion, retry/terminal-failure transitions, stale lock recovery and `Sqlite.decode[JobRecord]` queries |

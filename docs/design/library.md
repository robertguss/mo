# Where the library is implemented

**Draft, 12 October 2026.** This is a map of Mo's core library: every function
of reference section 14, and every operator that section 3 defines on a
built-in type, with where it is implemented. It belongs to Architecture v0.1
(`architecture.md`) and points into two of its contracts, `src/base/prims.def`
and `src/base/hostops.def` (`bytecode.md` sections 6 and 7). `make
library-map` checks that this map and those two tables agree.

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
| `compare`: the order `sort`, `sort_by`, `sort_by_desc`, `min` and `max` use, and `<` on tuples, lists, records and choice types | lowering | generated for each type with `Order`: the type's own `compare` where it has one; for the immediates of `bytecode.md` 4.4, `lt` and `eq`, except for `Float`, whose order is `cmp_float`, with not-a-number last; `Equal` for `Unit`; `Text.%compare`, `BigInt.%compare` and `Bytes.%compare`; tuples part by part from the left, lists element by element, records field by field and choice types case by case, in declaration order | the text forms and the order programs see are Mo functions, not VM operations (`bytecode.md` 8). Mo code in `lib/` orders with `compare`, not `<`, because `<` on floats follows IEEE 754 and leaves not-a-number unordered |
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

`bytecode.md` 4.4: everything on numbers but the operators is a primitive.

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `a + b`, `a - b`, `a * b` | instruction | `add`, `sub` and `mul` at `int` | |
| `a / b`, `a % b` | instruction | `div` and `rem` at `int` | |
| `-a` | instruction | `neg` at `int` | |
| `a == b`, `a != b` | instruction | `eq` at `int`, and `not` for `!=` | |
| `a < b`, `a <= b`, `a > b`, `a >= b` | instruction | `lt` and `le` at `int`; `>` and `>=` swap the operands | |
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
| `seconds` | primitive | `INT_SECONDS` | |
| `milliseconds` | primitive | `INT_MILLISECONDS` | |
| `minutes` | primitive | `INT_MINUTES` | |
| `hours` | primitive | `INT_HOURS` | |
| `days` | primitive | `INT_DAYS` | |

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

Every operation on text is a primitive (architecture section 7).

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
| `d * n`, with an `Int` | instruction | `mul_int` at `duration` | |
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
| `list(folder)` | host | `FILES_LIST` | |
| `exists?(path)` | host | `FILES_EXISTS` | |
| `within(folder)` | host | `FILES_WITHIN` | |
| `read_only` | host | `FILES_READ_ONLY` | |
| `Files.in_memory(files)` | host | `FILES_IN_MEMORY` | a fake, for spec code only (reference 12.4) |

## ReadFiles

`ReadFiles` is a capability type of its own (`bytecode.md` 2.1), so its
operations have lines of their own in `hostops.def`. The host runs them with
the same code as `Files`'s, on a handle marked read-only (`bytecode.md` 7).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `read_text(path)` | host | `READFILES_READ_TEXT` | |
| `read_bytes(path)` | host | `READFILES_READ_BYTES` | |
| `list(folder)` | host | `READFILES_LIST` | |
| `exists?(path)` | host | `READFILES_EXISTS` | |
| `within(folder)` | host | `READFILES_WITHIN` | |

## FileError

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `NotFound(path: p)`, and the other cases | instruction | `make_case`; `switch` and `case_field` | |
| `a == b`, `a != b` | generic | `equal`, and `not` for `!=` | |
| `to_text` | Mo | `lib/file-error.mo` | a type's own `to_text` is an ordinary function in its module (reference 9.2), and Mo expresses this one as one `case` |

## Clock

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `now` | host | `CLOCK_NOW` | |
| `Clock.fixed(time)` | host | `CLOCK_FIXED` | a fake, for spec code only (reference 12.4) |

## Random

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `int(low, high)` | host | `RANDOM_INT` | |
| `Random.seeded(seed)` | host | `RANDOM_SEEDED` | a fake, for spec code only (reference 12.4) |

## Unicode16

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `Unicode16.graphemes(text)` | primitive | `UNICODE16_GRAPHEMES` | |
| `Unicode16.display_width(text)` | primitive | `UNICODE16_DISPLAY_WIDTH` | |
| `Unicode16.normalize_nfc(text)` | primitive | `UNICODE16_NORMALIZE_NFC` | |
| `Unicode16.lowercase(text)` | primitive | `UNICODE16_LOWERCASE` | |
| `Unicode16.uppercase(text)` | primitive | `UNICODE16_UPPERCASE` | |

## Functions

Functions have no abilities (reference 9.2).

| Function | Where | Implemented by | Why |
| --- | --- | --- | --- |
| `f(a, b)`, calling a function value | instruction | `call_closure`, or `tail_call_closure` in tail position | |

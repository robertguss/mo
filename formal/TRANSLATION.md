# Translating Mo v0.1 to Core

This document says how every feature of the Mo Language Reference v0.1
(`docs/reference/README.md`) lowers to Mo Core, section by section in the
reference's numbering. The front end lowers a checked program: names are
resolved, every expression has its type, and every rule of the reference has
been checked. Core is what remains. The Core programs in `MoCore/Examples/`
were translated by hand following this document; their comments show each Mo
function next to its Core.

## 0. Notation

Core is defined in Lean (`MoCore/Syntax.lean`). This document writes it in a
compact text form:

| Text form | Lean (`Expr`) | Helper in `MoCore/DSL.lean` |
| --- | --- | --- |
| `42`, `12.50`, `"hi"`, `'c'`, `true`, `Unit`, `bytes[0x68, 0x69]` | `.lit` | `int`, `dec`, `str`, `chr`, `bool`, `unitE`, `byteE`, `bytesE` |
| `x` | `.var "x"` | `v` |
| `global x` | `.global "x"` | `global` |
| `let x = e in body` | `.let_` | `lets`, `seq` |
| `fn(x: T, ...) => body` | `.lam` | `lam` |
| `e(e1, ..., en)` (`e` a function value) | `.app` | `app` |
| `f(e1, ..., en)` (`f` a top-level function) | `.call` | `call` |
| `(e1, ..., en)`, `e.0` | `.tuple`, `.proj` | `tup`, `fst`, `snd` |
| `C(e1, ..., en)` | `.con T "C" [...]` | `mk`, `mkC`, `some_`, `ok_`, ... |
| `case e of C1(x, y) -> e1 \| C2 -> e2 end` | `.case_` | `caseOpt`, `caseRes`, `get` |
| `if c then a else b` | `.if_` | `ite`, `and_`, `or_` |
| `%op(e1, ..., en)` | `.prim` | `prim`, `iadd`, `top`, `lop`, `mop`, `sop`, `io`, ... |
| `stop "reason"` | `.stop` | `stop` |
| `fail detail` | `.fail` | `fail` |
| `join j(x: T, ...) = body in rest` | `.join` | `forList`, `forSet`, `whileLoop`, `forRange` |
| `jump j(e1, ..., en)` | `.jump` | `nextRound`, `breakLoop`, `ret` |

Primitives are written with their Lean names: `%int.add` is
`Prim.int .add`, `%cmp.decimal.lt` is `Prim.cmp .decimal .lt`, `%text.isEmpty`
is `Prim.text .isEmpty`, `%list.uncons` is `Prim.list T .uncons`, `%map.set` is
`Prim.map K V .set`, `%set.add` is `Prim.set T .add`, `%range.make` is
`Prim.range .make`, `%eq` is `Prim.eq T`, `%io.print` is `Prim.io .print`.
Their types are in `Prim.sig`.

Core is explicit where Mo infers: function, anonymous-function and join-point
parameters carry their types, and a constructor application names its type.
Names that start with `%` are made by the lowering; Mo names never do. A
program has data and function tables, ordered constant declarations and a
designated entry (`Program`). Each function records its module, kind and
required constant prefix. The program document also carries compiler-only
side tables (`TEXT.md`): file paths/spans, source binders and stable names,
module ownership/exposure, spec wording/links and promise condition lines.
Lowering copies them from the checked program and `mo_sources` into the Core
arena and reindexes them after every body rewrite. The Lean evaluator,
typing rules and proofs ignore them; the compiler never revisits checked Mo
or opens source to reconstruct them. The library's
choice types `Option(T)`, `Result(T, E)`, `Ordering` and `FileError` are
built in (`ctorsOf`), with their cases in the order reference 8 and 9.2
declare them: `None` before `Some`, `Ok` before `Error`, and `Less`, `Equal`,
`Greater`. `Entry` and `EntryKind` are built in too (10.5). A `case` lists
its arms in declaration order.

## 1. A first program

Functions keep their module as metadata; `##` comments are not in Core.
`main` is an ordinary function (section 11.2). The `for` loop becomes two
join points (section 7.5):

```mo
fn main(args: Args, console: Console)
  for word in args.list
    console.print(word.ascii_uppercase)
  end
end
```

```
fn main(args: Args, console: Console) : Unit =
  join w.done() = Unit in
  join w.loop(w.rest: List(Text)) =
    case %list.uncons(w.rest) of
      None -> jump w.done()
    | Some(w.p) -> let word = w.p.0 in
                   let w.rest = w.p.1 in
                   let %_ = %io.print(console, %text.upper(word)) in
                   jump w.loop(w.rest)
    end
  in jump w.loop(%io.argsList(args))
```

## 2. Source text

Files, comments, names and line breaks (2.1 to 2.4) are the front end's
business and leave no semantic operation in Core. The compiler side tables
retain filenames, byte spans, original binding names and spec lines (including
their internal comments/spacing); other comments and layout are not retained.
A Core variable is a resolved string, `?` and all (`failed?`), possibly renamed
by lowering; its debug name stays the source spelling. Functions keep their names; where a module
has several functions of one name (5.1), or a type customizes `to_text`, the
lowering qualifies the name: `LineError.to_text`.

Nongeneric imports introduce no Core operation. Both `Shapes.make(19)` and an
explicitly imported `make(19)` call the selected declaring function `Shapes.make`.
Likewise a returned record retains its declaring nominal name `Shapes.Box`,
even through a third module; an importing module's own `Box` is a different type.
Lowering consumes the checked symbol and `type.declaration` pointers, interpreting
choice owner/case indices in that declaration's module scope. It does not resolve
import spellings again or consume the checker's private import-use bookkeeping.
Core module ownership, exports (including opaque versus exposed contents), source
file indices, declaration/body spans and spec wording retain the declaring source.
Nongeneric source calls, first-class references, records, choices and snapshots
use the existing translation rules unchanged. Certified imported generic
function calls/values likewise reuse section 9's instances and scoped proofs,
including returned/captured references and three-module propagation. Certified
foreign generic nominal annotations, constructors, fields and choice patterns
reuse the declaring data layouts in sections 4 and 9. Foreign constant reads
and direct imported initializer references remain explicit checker E1103; this
integration does not add a new Core representation or reorder constants.

## 3. Values and types

| Mo | Core type | Core value |
| --- | --- | --- |
| `Bool` | `bool` | `bool b` |
| `Int` | `int` | `int n`, `n` within −2⁶³ to 2⁶³ − 1 |
| `BigInt` | `bigInt` | `bigInt n` |
| `Byte` | `byte` | `byte n`, `n` from 0 to 255 |
| `Decimal` | `decimal` | `decimal n`, meaning `n × 10⁻¹⁸`, within ±(2¹²⁷ − 1) |
| `Float` | `float` | `float bits` (IEEE 754) |
| `Text`, `Char` | `text`, `char` | `text s`, `char c` |
| `Bytes` | `bytes` | `bytes [b, ...]`, each `b` from 0 to 255 |
| `Unit` | `unit` | `unit` |
| `(A, B)` | `tuple [A, B]` | `tuple [a, b]` |
| `List(T)` | `list T` | `list [...]` |
| `Map(K, V)` | `map K V` | `map [(k, v), ...]`, in insertion order |
| `Set(T)` | `set T` | `set [x, ...]`, in insertion order, no element equal to another |
| `Time`, `Duration` | `time`, `duration` | microseconds (since 1970-01-01T00:00:00Z for a time) |
| `Range` | `range` | `range a b` for `a..b`, two `Int`s |
| a `struct` or `enum` | `data "Name"` | `con "Case" [fields]` |
| `Option(T)`, `Result(T, E)` | `option T`, `result T E` | `con "Some" [v]`, `con "Error" [e]`, ... |
| `Entry`, `EntryKind` | `entry`, `entryKind` | `con "Entry" [text name, con kind []]`; kind is `File`, `Folder`, `Link` or `Other` |
| `fn(A) : B` | `fn [A] B` | a closure |
| `Console`, `Files`, ... | `cap c` | a handle, `cap c folder` |

`Unicode16` has trusted primitive signatures in Core, not Unicode tables
(section 14 and the library decisions below).

### 3.1 Bool

`and`, `or` and `implies` skip their right side when the left decides, so
they lower to `if`; `not` is a primitive.

```mo
ready = in_stock and not on_hold
```

```
let ready = if in_stock then %not(on_hold) else false in ...
```

`a or b` is `if a then true else b`, and `a implies b` is
`if a then b else true`.

### 3.2 Whole numbers

An `Int` literal is a Core literal once the front end has read it
(`8_100_000_000`, `0xFF`); its typing requires it in range. The operators and
the library's whole-number functions are primitives that check their bugs:
overflow and division by zero stop the program, they never wrap.

The C lowerer folds unary minus on a numeric immediate before constructing
the scalar. In particular, `-9223372036854775808` is one negative Int literal,
not an out-of-range positive Int followed by `%int.neg`.

```mo
q = -7 / 2                     # -3
r = -7 % 2                     # -1
fits = a.checked_add(b)        # an Option(Int)
hash = h.wrapping_mul(31)
```

```
let q = %int.div(%int.neg(7), 2) in
let r = %int.rem(%int.neg(7), 2) in
let fits = %int.checkedAdd(a, b) in
let hash = %int.wrappingMul(h, 31) in ...
```

`a / b` and `a % b` round toward zero (`Int.tdiv`, `Int.tmod`);
`floor_div` and `floor_mod` round toward negative infinity. `BigInt` has its
own primitives (`%big.add`, ..., `%big.toInt`), and `n.to_big` is
`%int.toBig(n)`.

A `Byte` holds 0 to 255 and has no arithmetic (reference 3.2, 9.2).
`n.to_byte` is `%int.toByte(n)`, an `Option(Byte)`; `b.to_int` is
`%byte.toInt(b)`; and `b.to_text` is `%byte.toText(b)`, the byte's decimal
digits, as an `Int`'s are. Bytes compare with `%cmp.byte.lt` and the others,
and `==` is `%eq`. A whole-number literal where a `Byte` is expected is a byte
literal, as one where a `Decimal` is expected is a decimal (3.3), so in
`b < 128`, with `b` a `Byte`, the `128` is the byte `128`. Typing requires a
byte literal from 0 to 255, and the front end reports one outside that range.

```mo
low? = b < 128
code = b.to_int
```

```
let low? = %cmp.byte.lt(b, 128) in
let code = %byte.toInt(b) in ...
```

### 3.3 Decimal and Float

A decimal literal is its exact count of 10⁻¹⁸: `19.99` is
`decimal 19990000000000000000`. A literal takes the type its context needs;
the front end has decided it, so in `price * 2` with a `Decimal` price the
`2` is the decimal `2`:

```mo
total = price * 2
third = 1.00 / 3               # 0.333333333333333333, ties to even
label = share.round(2).to_fixed(2)
cents = count.to_decimal / 100
```

```
let total = %dec.mul(price, 2) in
let third = %dec.div(1.00, 3) in
let label = %dec.toFixed(%dec.round(share, 2), 2) in
let cents = %dec.div(%int.toDecimal(count), 100) in ...
```

Multiplication and division round to 18 places with ties to even
(`roundHalfEven`); `round` and `to_fixed` round ties away from zero.
`Float` values are IEEE 754 doubles, with `%float.add` and the others.
The C lowerer converts Float spellings in a private C numeric locale with
nearest-even rounding, preserving signed zero and restoring the caller's
rounding state. Decimal spellings instead become exact scaled integers.

### 3.4 Text, Char and Bytes

Escapes and text on several lines are the front end's: Core gets the
resulting text.
The staged C checker retains literal spellings. The lowerer decodes escapes,
normalizes triple-text CRLFs and removes the closing delimiter's indentation;
the resulting UTF-8 bytes belong to the Core arena.

Interpolation lowers to `%text.concat` of the parts, each converted to its
text form:

| Part's type | Its text form in Core |
| --- | --- |
| `Text` | itself |
| `Char` | `%char.toText(c)` |
| `Int` | `%int.toText(n)` |
| `BigInt` | `%big.toText(n)` |
| `Byte` | `%byte.toText(b)`, its decimal digits |
| `Decimal` | `%dec.toText(d)`, the shortest exact spelling |
| `Float` | `%float.toText(x)`: `nan`, `inf`, `-inf`, or the shortest round-tripping decimal (numeric decisions below) |
| `Bool` | `if b then "true" else "false"` |
| `Time`, `Duration` | `%time.toText(t)`, RFC 3339 in UTC with `Z` and the shortest fraction of a second; `%time.durToText(d)`, in seconds, as `1.5s` |
| `Range` | `%range.toText(r)`, its ends with `..` between them, as in `0..3` (reference 9.2) |
| `Set(T)` | a call of `to_text[Set(T)]`, which the lowering derives: the elements in insertion order, written as a list's, then `.to_set`, as in `[1, 2].to_set`, and `[].to_set` for the empty set (`Lib.setText`, given how an element is written inside another value) |
| a type with its own `to_text` | a call of it, such as `LineError.to_text(problem)` |
| `FileError` | `FileError.to_text(e)` (`MoCore/Lib.lean`) |
| any other record, choice type, tuple, list or map | a call of `to_text[T]`, a Core function the lowering derives per type, which quotes texts and characters inside, and writes ranges inside as `0..3` |

```mo
greeting = "Hello, #{name or "you"}! You have #{count} new messages."
```

```
let greeting =
  %text.concat("Hello, ",
    %text.concat(case name of None -> "you" | Some(%or) -> %or end,
      %text.concat("! You have ",
        %text.concat(%int.toText(count), " new messages.")))) in ...
```

`x.to_text` (reference 9.2, 14) is the same text as `"#{x}"`, so it lowers to
the same Core as the part `#{x}` in the table above: `count.to_text` is
`%int.toText(count)`, `name.to_text` for a `Text` is `name` itself,
`problem.to_text` is `LineError.to_text(problem)`, and at a type whose text
form the lowering derives, it calls the one `to_text[T]` derived for that type,
which interpolation calls too. So the two never differ. The Lean model doesn't
generate `to_text[T]` yet, except a set's, `Lib.setText`, which takes as a
parameter how an element is written inside another value; none of the
samples needs it.

In C lowering, interpolation's checked operands are only the hole expressions,
in written order. Literal HEAD/MID/TAIL parts remain syntax and receive their own
Core spans after decoding. Triple parts share the final delimiter's indentation
and normalize CRLFs. Lowering binds each hole's text conversion before evaluating
the next hole, so a custom `to_text` that stops cannot be overtaken by a later
hole's evaluation. Explicit `to_text`, interpolation and entry error printing
use the same selected proof, rather than looking up a customizer again.
Derived helpers reserve their function before following proof parts and use a
bounded, memoized FIFO queue rather than C recursion. Tuple parts convert in
declaration order. Native Text/Char parts are quoted within structural values;
escaping processes original slashes first, then delimiters, interpolation
markers and control characters, so newly inserted escapes are not doubled.

Qualified native scalar calls, such as `Int.to_text`, use the same scalar
conversion as a selected native Text proof. A same-name ordinary source helper
still calls its checked source symbol, so `to_text(7)` may return an Int while
`29.to_text` returns Text in the same module. `Text.byte_count` lowers to
`%text.byteCount`, counting UTF-8 bytes rather than characters. A supported
qualified native used as a function value, such as `Int.to_text` or
`Text.byte_count`, becomes a `.ref.ROW` lambda adapter (5.6) applying the same
primitive to its parameters. Unsupported natives still refuse E1150 there.
Namespace calls, such as `Int.to_text(29)`, retain a pure native callee row in
the checked graph; lowering skips it before applying the primitive to the
written arguments. Receiver dots, such as `29.to_text`, have no callee row.

The text functions of section 14 are primitives (`%text.lines`,
`%text.split`, ...); see section 14 below.

`text.bytes` is `%text.bytes(text)`, the text's UTF-8 encoding, with as many
bytes as `byte_count` counts. On `Bytes`, `size` is `%bytes.size`; `get(i)` is
`%bytes.get`, `None` below 0 or past the end, as for a list; and
`decode_utf8` is `%bytes.decodeUtf8`, which gives `None` unless the bytes are
well-formed UTF-8 as table 3-7 of the Unicode Standard defines it: no
overlong form, no surrogate (U+D800 to U+DFFF), nothing above U+10FFFF, and
no sequence cut short. A byte order mark is the character U+FEFF, kept like
any other, as Mo never normalizes text silently (reference 3.4). `Bytes` has
`Equal`, `Hash` and an order, byte by byte, in which a sequence comes before
any longer one it begins, as a list does; it has no text form (reference
9.2), so it can't be interpolated.

```mo
size = text.bytes.size
back = text.bytes.decode_utf8 or ""
```

```
let size = %bytes.size(%text.bytes(text)) in
let back = case %bytes.decodeUtf8(%text.bytes(text)) of None -> "" | Some(%or) -> %or end in ...
```

Mo has no way to write a `Bytes` value, but Core has a literal for one,
`bytes[0x68, 0x69]`, as the bytecode has constants for them (`bytecode.md`
3.1). The lowering never makes one from Mo; the model's checks use it to
build bytes that aren't UTF-8.

### 3.5 Unit

`Unit` is the literal `Unit` of type `unit`. A statement whose value is
dropped is `let %_ = e in rest` (`seq` in the DSL).

### 3.6 Tuples

A tuple is `tuple`, `pair.0` is `pair.0`, and a tuple pattern in a binding
projects:

```mo
(low, high) = bounds(readings)
spread = high - low
```

```
let %t = bounds(readings) in
let low = %t.0 in
let high = %t.1 in
let spread = %int.sub(high, low) in ...
```

### 3.7 Lists

A list literal is `%list.lit n` applied to its `n` elements; the list
functions are primitives or Core functions (section 14):

```mo
first = items.first
third = items.get(2)
longer = items.append(4)
```

```
let first = %list.first(items) in
let third = %list.get(items, 2) in
let longer = %list.append(items, 4) in ...
```

### 3.8 Maps and sets

A map is an association list in insertion order: `%map.set` replaces the
value of an existing key in place and adds a new key at the end, and `%map.remove`
keeps the others' order. Equality (`%eq`) compares entries in any order.

A map literal with `n` entries is `%map.lit n` applied to its keys and values
in source order, `k1, v1, k2, v2, ...`. It computes all of them, left to
right, builds the map in that order, and stops with the bug `repeatedKey` if
two keys turn out equal (reference 3.8, 13.2). Keys the front end can see are
equal, such as two equal literals or the same name twice, are an error before
Core. `{}` is
`%map.empty()`. `List.to_map` is a different primitive, `%map.ofList`: for a
key given twice it keeps the later value, in the key's first place
(reference 14).

```mo
stock = {"apples": 3}
more = stock.set("pears", 5)
count = more.get("apples") or 0
for (fruit, n) in more
  console.print("#{fruit}: #{n}")
end
```

```
let stock = %map.lit1("apples", 3) in
let more = %map.set(stock, "pears", 5) in
let count = case %map.get(more, "apples") of None -> 0 | Some(%or) -> %or end in
join m.done() = ... in
join m.loop(m.rest: List((Text, Int))) =
  case %list.uncons(m.rest) of
    None -> jump m.done()
  | Some(m.p) -> let %e = m.p.0 in let m.rest = m.p.1 in
                 let fruit = %e.0 in let n = %e.1 in
                 let %_ = %io.print(console, %text.concat(fruit, %text.concat(": ", %int.toText(n)))) in
                 jump m.loop(m.rest)
  end
in jump m.loop(%map.entries(more))
```

A set is a value of its own type, `set T`, as in the VM (`bytecode.md` 2.1):
its elements in insertion order, none equal to another, which value typing
requires (`ValTy.set`). `Set.empty` is `%set.empty()`, and `items.to_set` is
`%set.ofList(items)`, which keeps each element in its first place, as
`items.unique` does. `s.size`, `s.empty?`, `s.has?(x)`, `s.add(x)`,
`s.remove(x)` and `s.to_list` are `%set.size`, `%set.isEmpty`, `%set.has`,
`%set.add`, `%set.remove` and `%set.toList`. `add` puts a new element last,
and leaves the set as it is when an equal element is there, so that element
keeps its place, as `%map.set` keeps a key's. `remove` keeps the others'
order, and an element removed and added again goes last. `a.union(b)` is
`%set.union(a, b)`: the elements of `a` in their order, then those of `b`
that `a` doesn't hold, in `b`'s order, as if each were added to `a` in turn.
`a.intersection(b)` and `a.difference(b)` are `%set.intersection(a, b)` and
`%set.difference(a, b)`: the elements of `a` that `b` holds, or doesn't, in
`a`'s order. Equality (`%eq`) compares elements in any order. A `for` loop
visits the elements in insertion order (7.5):

```mo
seen = [3, 1, 3].to_set            # 3, then 1
more = seen.add(2).add(3)          # 3, 1, 2: 3 keeps its place
both = more.union([4, 1].to_set)   # 3, 1, 2, 4
for n in both
  console.print("#{n}")
end
```

```
let seen = %set.ofList(%list.lit3(3, 1, 3)) in
let more = %set.add(%set.add(seen, 2), 3) in
let both = %set.union(more, %set.ofList(%list.lit2(4, 1))) in
join s.done() = ... in
join s.loop(s.rest: List(Int)) =
  case %list.uncons(s.rest) of
    None -> jump s.done()
  | Some(s.p) -> let n = s.p.0 in let s.rest = s.p.1 in
                 let %_ = %io.print(console, %int.toText(n)) in
                 jump s.loop(s.rest)
  end
in jump s.loop(%set.toList(both))
```

Inventory's `first_by` keeps the keys it has seen in a set. The last section
of this document lists what the model decided where the reference leaves a
question about sets open.

### 3.9 Time and Duration

Both count whole microseconds. `5.seconds` and the other conversions check
for overflow; adding a duration to a time, or subtracting one from it, stops
outside the years 1 to 9999, and `checked_add` returns an `Option` instead,
`None` outside them (a negative duration goes back in time).

```mo
expires = now + 30.minutes
an_hour_ago = now - 1.hours
late? = now - order.placed > 2.days
```

```
let expires = %time.add(now, %int.minutes(30)) in
let an_hour_ago = %time.subDur(now, %int.hours(1)) in
let late? = %cmp.duration.gt(%time.sub(now, <order.placed>), %int.days(2)) in ...
```

`-` with a `Time` on the left is `%time.subDur` when the right side is a
`Duration`, giving a time, and `%time.sub` when it is a `Time`, giving a
duration, which never overflows.

## 4. Records and choice types

### 4.1 Records

A `struct` is a data declaration with one constructor, named like the type,
whose fields are the record's in declaration order. Building a record gives
the fields in that order; reading a field is a one-arm `case` that binds every
field; assigning a field builds a new record (section 6.2).

```mo
struct Point
  x: Decimal
  y: Decimal
end

origin = Point(x: 0, y: 0)
sum = origin.x + origin.y
```

```
data Point = Point(x: Decimal, y: Decimal)

let origin = Point(0, 0) in
let sum = %dec.add(case origin of Point(%x, %y) -> %x end,
                   case origin of Point(%x, %y) -> %y end) in ...
```

Fields are computed in the order they are written, whatever the order of
their declaration (reference 4.1), so fields named out of declaration order
are first bound in source order:
`Point(y: f(), x: g())` is `let %y = f() in let %x = g() in Point(%x, %y)`.

### 4.2 Choice types

An `enum` is a data declaration with one constructor per case, in order. A
case's one unnamed value is a field named `0`. A `case` on it has one arm per
constructor, in declaration order, each binding the constructor's fields:

```mo
enum Shape
  Dot
  Circle(Decimal)
  Rectangle(width: Decimal, height: Decimal)
end

fn area(shape: Shape) : Decimal
  case shape
    Dot: 0
    Circle(r): 3.14159 * r * r
    Rectangle(width: w, height: h): w * h
  end
end
```

```
data Shape = Dot | Circle(0: Decimal) | Rectangle(width: Decimal, height: Decimal)

fn area(shape: Shape) : Decimal =
  case shape of
    Dot -> 0
  | Circle(r) -> %dec.mul(%dec.mul(3.14159, r), r)
  | Rectangle(w, h) -> %dec.mul(w, h)
  end
```

A generic type is declared once for each type it is used at (section 9.1):
`Tree(Int)` is the declaration `Tree(Int) = Leaf | Node(left: Tree(Int),
value: Int, right: Tree(Int))`. Recursive types need nothing more:
`MoCore/Examples/JsonQuery.lean` declares `Json` with fields `List(Json)` and
`Map(Text, Json)`.

## 5. Functions

### 5.1 Definitions

A function is a `FunDecl`: its name, module, kind, typed parameters, result
type, body and required constant prefix (10.4).
A body's statements become nested `let`s, and its value is its last line. A
`return` with a guard, at the top of a body, is an `if` whose other branch is
the rest of the body:

```mo
fn discount(total: Decimal, member?: Bool) : Decimal
  return 0 if not member?
  return total * 0.10 if total > 100
  total * 0.05
end
```

```
fn discount(total: Decimal, member?: Bool) : Decimal =
  if %not(member?) then 0 else
  if %cmp.decimal.gt(total, 100) then %dec.mul(total, 0.10) else
  %dec.mul(total, 0.05)
```

A `return` deeper inside, such as in a loop or in a `case` whose value is
bound, jumps to a join point around the whole body:
`join %return(%r: R) = %r in body`, with `return e` as `jump %return(e)`.
When the `return` is in tail position already, as above, no join point is
needed.

The C lowerer's initial CPS pass conservatively installs this join whenever
a return is reachable in the function, excluding nested lambda bodies. A
function or lambda with no reachable return has no return join. Expression
walks and continuation expansion use explicit arena-backed work lists, never
recursive C calls; the generated Core path limit can refuse an expanded body.

### 5.2 Calls

A call of a top-level function is `call`: `shipping(2.5, false)` is
`shipping(2.5, false)`. Arguments are positional in Core as in Mo.

### 5.3 Recursion

Recursion is a `call` of the function itself; mutual recursion works through
the program's function table:

```mo
fn sum_to(n: Int, total: Int) : Int
  if n == 0: total else: sum_to(n - 1, total + n)
end
```

```
fn sum_to(n: Int, total: Int) : Int =
  if %eq(n, 0) then total else sum_to(%int.sub(n, 1), %int.add(total, n))
```

A call in tail position (the value of the body, of an `if` branch, of a
`case` arm, of a `let` body or of a join point) is a tail call; the bytecode
compiler marks it so it uses no stack (reference 5.3). The Core evaluator
treats all calls alike.

### 5.4 Dot calls

A dot call is resolved by the front end to the function it calls, and Core
just calls it, or applies the primitive:

```mo
size = text.trim.lines.size
same = words.shout
```

```
let size = %list.size(%text.lines(%text.trim(text))) in
let same = shout(words) in ...
```

### 5.5 Generic functions

A generic function is instantiated at each list of types it is used with,
and each instance is an ordinary Core function named with its types
(section 9):

```mo
fn largest(items: List(T)) : Option(T) where T: Order
  items.sort.last
end
```

used at `Int`, is

```
fn largest[Int](items: List(Int)) : Option(Int) = %list.last(List.sort_by[Int,Int](items, fn(x: Int) => x))
```

A generic function can't call itself, directly or through other functions,
with a type parameter standing for a bigger type built from that parameter,
such as `depth` on a `Tree(T)` calling `depth` on a `Tree(List(T))`
(reference 5.5). So instantiation always ends.

### 5.6 Functions as values

An anonymous function is a closure; its parameter types are the ones the
front end inferred. A named function used as a value is an anonymous function
that calls it. A field that holds a function is read, then applied.

```mo
taxed = prices.map(fn(p) p * 1.2 end)
also = prices.map(with_tax)
cells = rows.map(column.cell)
```

```
let taxed = List.map[Decimal,Decimal](prices, fn(p: Decimal) => %dec.mul(p, 1.2)) in
let also = List.map[Decimal,Decimal](prices, fn(p: Decimal) => with_tax(p)) in
let cells = List.map[T,Text](rows, case column of Column(T)(%title, %cell, %numeric?) -> %cell end) in ...
```

Anonymous functions never capture capabilities and never assign a `var`
(reference 5.6); the front end checks that, so Core needs no rule for it.
A closure captures the values of the names it reads when it is made. An
anonymous function may read a `var` only if nothing assigns the `var` after
the function is made, including in a later round of a loop around it; the
front end rejects the rest (reference 5.6), so a closure never has to choose
between the value it was made with and a later one.

The C lowerer prefixes a capturing lambda with local aliases in the checked
first-use order: `fn() => let y = y in let x = x in ...`. Each right-hand
name denotes the outer binding, before its local alias shadows it. Core's
closure creation snapshots that ordered free environment by value. Prefixes
prevent CPS or pattern rewrites from changing the capture order; their source
metadata keeps the capture names and the lambda's responsible span.

The declaration symbol retained on a checked postfix call is provenance,
not necessarily its call target. A call whose callee is a zero-operand reference
to that declaration emits `call`; a call whose callee is another call emits
`app`. For `make(19)()` the translation is `let %f = make(19) in app %f ()`,
not a second call to `make`. The returned closure keeps its escaped parameter
values after `make` returns. Named-function value adapters still call their
own declarations; no new closure representation or constant dependency is needed.

`return` and `try` inside an anonymous function leave that anonymous function
(reference 5.6). The lowering puts its `%return` join point (5.1) inside the
anonymous function's body, and Core couldn't do otherwise: `lam`'s body is
typed with no join points in scope, so no jump leaves it.

## 6. Names, bindings and variables

### 6.1 Bindings

`name = value` is `let name = value in` the rest of the block. A tuple
binding projects (3.6); `_` is `let %_ = e in`.

```mo
subtotal = order.lines.map(fn(line) line.price end).sum
tax = subtotal * 0.2
```

```
let subtotal = List.sum[Decimal](List.map[OrderLine,Decimal](<order.lines>, fn(line: OrderLine) => <line.price>)) in
let tax = %dec.mul(subtotal, 0.2) in ...
```

### 6.2 Variables

A `var` becomes a sequence of bindings: each assignment binds the name again
(Core allows shadowing). Assigning a field builds a new record with the field
replaced. Where control flow joins, the `var`s assigned on the way are passed
to the join point that continues (sections 7.1 and 7.5).

```mo
var cart = Cart(owner: "ana", items: [], total: 0)
cart.items = cart.items.append(item)
cart.total += item.price
```

```
let cart = Cart("ana", %list.lit0(), 0) in
let cart = let %%new = %list.append(<cart.items>, item) in
           case cart of Cart(%owner, %items, %total) -> Cart(%owner, %%new, %total) end in
let cart = let %%new = %dec.add(<cart.total>, <item.price>) in
           case cart of Cart(%owner, %items, %total) -> Cart(%owner, %items, %%new) end in ...
```

`x += e` is `x = x + e`. The new value is computed before the record is taken
apart, so its own reads of the fields see the old record, as in Mo.

## 7. Control flow and patterns

### 7.1 if

An `if` with a value is Core's `if`. An `if` statement followed by more
statements binds the rest of the block as a join point that takes the `var`s
the branches assign; when the branches assign nothing, the statement is just
dropped:

```mo
var label = "small"
if n > 100
  label = "large"
end
console.print(label)
```

```
let label = "small" in
join %k(label: Text) = %io.print(console, label) in
if %cmp.int.gt(n, 100) then jump %k("large") else jump %k(label)
```

```mo
if args.list.size > 1
  console.print(row(total, "total"))
end
```

```
let %_ = if %cmp.int.gt(%list.size(%io.argsList(args)), 1)
         then %io.print(console, row(total, "total")) else Unit in ...
```

`else if` chains nest; the one-line form is the same `if`.

### 7.2 case and 7.3 Patterns

A `case` lowers to a decision tree (the pattern-matrix method the front end
uses for exhaustiveness): each test looks at one level of one value, so Core
needs only one-level `case`, `if` and `%eq`.

- A constructor pattern is an arm of a one-level `case`, which lists every
  constructor of the type, in declaration order; nested patterns become
  nested `case`s on the fields.
- A literal pattern (`42`, `"GET"`, `'x'`, `true`) is an `%eq` test.
- `_` and a name test nothing; a name is a `let`.
- `A | B` is two arms with the same body (or two jumps to one join point).
- A guard is an `if` in the arm; when it fails, control goes on to the next
  arms, which become a join point `%next()` when several places reach them.
- A tuple pattern projects; a record pattern with named fields reads them.
- A list pattern is a chain of `%list.uncons`: `[]` is `None`, `[p, q]` is two
  `Some`s then `None`, and `[first, ..rest]` binds the tail.

The C decision tree tests alternatives in written order. Each arm has one
success join taking its canonical pattern binders and live outer vars, so
alternatives share the body and preserve one source identity per lambda.
A failed pattern or false guard tries the next alternative, including another
alternative of the same written arm. A guarded alternative group uses an
index-taking dispatch join and one guard join: guard failure resumes dispatch
at the following alternative. Failure continuations are joins, rather than
copies of the remaining tree. Constructor arms always follow declaration
order, including synthesized failure arms. An unreachable final failure is a
stop; exhaustiveness has already been proved by checking. Is/and/implies in a
condition compile around the success continuation, rather than first making
an intermediate Bool that would lose the pattern's true-path bindings.
Shared nontrivial condition/pattern continuations are joins too. Only atomic
leaves or small primitives/jumps with atomic arguments are copied, so chained
conditions grow linearly at fixed constructor width and each lambda occurs
once in the metadata preorder. Arguments evaluate in written order; atoms
pass directly, except mutable reads before later non-atomic operands are
snapshotted. Discarded Unit/atom statements add no let. Exit jumps retain the
return/break/continue span, and loop fall-through retains the loop span.

The reference's example:

```mo
message = case outcome
  Ok(order): "placed order #{order.id}"
  Error(UnknownItem(name: n)): "no such item: #{n}"
  Error(BadQuantity(problem)):
    detail = describe(problem)
    "bad quantity: #{detail}"
end
```

```
let message =
  case outcome of
    Ok(order) -> %text.concat("placed order ", %int.toText(<order.id>))
  | Error(%e) ->
      case %e of
        BadQuantity(problem) -> let detail = describe(problem) in %text.concat("bad quantity: ", detail)
      | UnknownItem(n) -> %text.concat("no such item: ", n)
      end
  end in ...
```

A list pattern with a fallback arm, from `expenses.mo`'s `parse_line`:

```mo
case line.split(",").map(fn(field) field.trim end)
  [_, category, amount]: <arm>
  fields: Error(WrongFieldCount(found: fields.size))
end
```

```
let fields = List.map[Text,Text](%text.split(line, ","), fn(field: Text) => %text.trim(field)) in
join %other() = Error(WrongFieldCount(%list.size(fields))) in
case %list.uncons(fields) of
  None -> jump %other()
| Some(%m0) -> let _ = %m0.0 in
    case %list.uncons(%m0.1) of
      None -> jump %other()
    | Some(%m1) -> let category = %m1.0 in
        case %list.uncons(%m1.1) of
          None -> jump %other()
        | Some(%m2) -> let amount = %m2.0 in
            case %list.uncons(%m2.1) of
              None -> <arm>
            | Some(%_) -> jump %other()
            end
        end
    end
end
```

Text patterns and a guard, from `todo.mo`'s `parse_command`, compare the
first element with each literal in turn and send every failure to the last
arm, the join point `%none`; see `MoCore/Examples/Todo.lean`.

### 7.4 is

`value is Pattern` in a `Bool` position is a `case` giving `true` or
`false`. Guarding an `if` or a `while`, its bindings reach the guarded block,
so the pattern's `case` holds the block, and every failing branch jumps to
the `else` part:

```mo
if reply is Ok(body) and not body.empty?
  console.print(body)
end
```

```
join %else() = <rest> in
case reply of
  Ok(body) -> if %not(%text.isEmpty(body))
              then let %_ = %io.print(console, body) in jump %else()
              else jump %else()
| Error(%_) -> jump %else()
end
```

`while console.read_line is Some(line)` is a loop whose body begins with
`case %io.readLine(console) of None -> <leave the loop> | Some(line) -> ...`
(`MoCore/Examples/JsonQuery.lean`, `read_input`).

### 7.5 Loops

A loop is two join points. `l.loop` takes the loop's position and the `var`s
the body assigns, and runs one round; `l.done` takes the `var`s and runs what
follows the loop. Every path through the body ends by jumping: to `l.loop`
for the next round (the end of the body, or `continue`), or to `l.done`
(`break`). A `for` over a list takes it apart with `%list.uncons`; over a map,
its `%map.entries`; over a set, its elements in insertion order,
`%set.toList` (3.8); over a range, an `Int` that counts from the range's
start while below its end.

```mo
var total = 0
for i in 0..10
  continue if i % 2 == 0
  total += i
end
```

```
let total = 0 in
let r.range = %range.make(0, 10) in
let r.end = %range.end(r.range) in
join r.done(total: Int) = <what follows> in
join r.loop(i: Int, total: Int) =
  if %cmp.int.lt(i, r.end) then
    if %eq(%int.rem(i, 2), 0) then jump r.loop(%int.add(i, 1), total)
    else jump r.loop(%int.add(i, 1), %int.add(total, i))
  else jump r.done(total)
in jump r.loop(%range.start(r.range), total)
```

```mo
var remaining = 100
var halvings = 0
while remaining > 1
  remaining = remaining / 2
  halvings += 1
end
```

```
let remaining = 100 in
let halvings = 0 in
join w.done(remaining: Int, halvings: Int) = <what follows> in
join w.loop(remaining: Int, halvings: Int) =
  if %cmp.int.gt(remaining, 1)
  then jump w.loop(%int.div(remaining, 2), %int.add(halvings, 1))
  else jump w.done(remaining, halvings)
in jump w.loop(remaining, halvings)
```

A range is a value of type `Range`, two `Int`s (architecture 4.3): `a..b` is
`%range.make(a, b)`, which computes `a`, then `b`, and is never a bug, so
`r = 0..10` is `let r = %range.make(0, 10) in`. `for i in r` binds the range,
reads its end once with `%range.end`, as above, and starts counting at
`%range.start(r)`; `%range.start` and `%range.end` are Core's way to take a
range apart, as `%list.uncons` takes a list apart, and Mo has neither. The
body runs only while `i` is below the end, so `%int.add(i, 1)` is at most the
end and never overflows, even in a range that ends at the largest `Int`.
A range whose end isn't above its start is empty: the loop runs no round.
`r.to_list` is `%range.toList(r)`, and `r.contains?(n)` is
`%range.contains(r, n)`, true when `r`'s start `<= n` and `n <` its end.
Reference 9.2 gives `Range` `Equal`, `Hash` and a text form but no order: two
ranges are equal when their ends are, so `5..2 != 6..2`, though both are
empty; a range can be a map key or a set member; and its text form, `0..3`,
is `%range.toText` (3.4).

`break` is `jump l.done(vars)`, `continue` is `jump l.loop(next position,
vars)`, and `return e` inside a loop is `jump %return(e)` (section 5.1), or
just `e` when the loop is in tail position of the body. A join body may jump
to an outer join point, so `break` and `return` leave nested loops directly.
The typing rules allow jumps only in tail position, which is what makes this
correct: a jump never has to come back.

## 8. Failures and missing values

`Option` and `Result` are the built-in data types `option T` and
`result T E`.

### 8.1 Options

`x or default` is a `case` that evaluates `default` only for `None`, as
reference 8.1 and 13.1 say (like the Bool `or`). `x.or_fail(e)` is a library
call, so `x` and `e` are both evaluated first:

```mo
city = address.city or "unknown"
found = users.get(id).or_fail(NoUser(id: id))
```

```
let city = case <address.city> of None -> "unknown" | Some(%or) -> %or end in
let found = let %opt = %map.get(users, id) in
            let %fail = NoUser(id) in
            case %opt of None -> Error(%fail) | Some(%x) -> Ok(%x) end in ...
```

### 8.2 Results and declared failures

A function that can fail returns a `Result`, built with its constructors:

```mo
fn parse_quantity(text: Text) : Result(Int, QuantityError)
  trimmed = text.trim
  return Error(Empty) if trimmed.empty?
  case trimmed.to_int
    Some(n) if n < 0: Error(Negative(value: n))
    Some(n): Ok(n)
    None: Error(NotANumber(text: trimmed))
  end
end
```

```
fn parse_quantity(text: Text) : Result(Int, QuantityError) =
  let trimmed = %text.trim(text) in
  if %text.isEmpty(trimmed) then Error(Empty) else
  case %text.toInt(trimmed) of
    None -> Error(NotANumber(trimmed))
  | Some(n) -> if %cmp.int.lt(n, 0) then Error(Negative(n)) else Ok(n)
  end
```

### 8.3 Passing failures upward: try

`try e` takes the value out of an `Ok` (or `Some`), or returns the failure
from the function. The front end first names the parts of the enclosing
expression that are evaluated before the `try` (`let`s, keeping left-to-right
order), then the `try` becomes a `case` whose `Ok` arm holds the rest of the
computation. The `Error` arm returns: directly when the `case` is in tail
position, otherwise by `jump %return(...)`. When the failure types differ,
the failure is wrapped in the one case of the function's failure type that
holds it as its unnamed value (8.3's automatic wrapping):

```mo
fn parse_line(line: Text, catalog: Map(Text, Item)) : Result(OrderLine, OrderError)
  parts = line.split(",")
  name = try parts.get(0).or_fail(UnknownItem(name: line))
  quantity = try parse_quantity(parts.get(1) or "")
  item = try catalog.get(name).or_fail(UnknownItem(name: name))
  Ok(OrderLine(item: item, quantity: quantity))
end
```

```
fn parse_line(line: Text, catalog: Map(Text, Item)) : Result(OrderLine, OrderError) =
  let parts = %text.split(line, ",") in
  case <%list.get(parts, 0).or_fail(UnknownItem(line))> of
    Ok(name) ->
      case parse_quantity(case %list.get(parts, 1) of None -> "" | Some(%or) -> %or end) of
        Ok(quantity) ->
          case <%map.get(catalog, name).or_fail(UnknownItem(name))> of
            Ok(item) -> Ok(OrderLine(item, quantity))
          | Error(%err) -> Error(%err)
          end
      | Error(%err) -> Error(BadQuantity(%err))
      end
  | Error(%err) -> Error(%err)
  end
```

A `try` inside an expression keeps the order of evaluation; from `json-query.mo`:
`value = value * 16 + try hex_digit_value(digit)` inside a loop is

```
let %left = %int.mul(value, 16) in
case hex_digit_value(digit) of
  None -> None
| Some(%digit) -> jump d.loop(d.rest, %int.add(%left, %digit))
end
```

where `None` returns from the function, because the loop is in tail
position. `try` on an `Option` works the same in a function returning an
`Option`. The samples `account`, `todo` (with wrapping into
`Storage(FileError)`) and `notes-tool` (into `Unreadable(FileError)`) show
both kinds.

The C lowerer performs the naming and lazy continuation rewrite itself from
the checked operand graph. It evaluates the input once, then emits a one-level
case in native constructor order: None/Some or Ok/Error. Its failure arm jumps
to the containing function or lambda's return join, even when more arithmetic
or a constructor follows the `try`. The checker supplies the exact failure
constructor and optional wrapper; lowering never searches for one. A wrapper's
type is the substituted enclosing Result's error type, so generic nominal
identity agrees with the checked result. Its checked type-only requirement uses
(`operand = UINT32_MAX`, keyed by wrapper-scoped parameter and ability) must
resolve in the supplying function instance's proof environment before emission;
an unresolved proof refuses E1150. Direct propagation has no wrapper arguments
or requirement uses. Option `or` similarly evaluates its
default only in the None arm and passes Some's value to the continuation.

### 8.4 Can't happen

`stop "reason"` is Core's `stop`; it fits any type:

```mo
rate = case rates.get(currency)
  Some(r): r
  None: stop "every currency was checked against the rate table at startup"
end
```

```
let rate = case %map.get(rates, currency) of
             None -> stop "every currency was checked against the rate table at startup"
           | Some(r) -> r
           end in ...
```

## 9. Generics and abilities

### 9.1 Generic types

A generic type is declared once per list of types it is used with, under a
name that includes them, such as `Column(Product)`; a generic function is
instantiated likewise (5.5). Core has no type variables. In the Lean model
both are Lean functions from types to declarations: `Column (t : Ty)` and
`topBy (t k : Ty)` in `MoCore/Examples/Inventory.lean`, `Lib.map (a b : Ty)`
in `MoCore/Lib.lean`.

The C implementation distinguishes nominal data from executable instances.
A generic data declaration is canonical program-wide by (defining declaration,
concrete type list) and owned by the defining module. If `Lib.make()` returns
`Lib.Box(Int)`, a requester-specialized function must accept that same nominal
type; making requester-specific data names would break the call's Core type.
Written imported nominal uses retain that same declaration. Field substitution
uses its record signature, or the first case's signature for a choice, with
abstract keys in the type's explicit parameter declaration order rather than
the importing function's A-Z order. Cases retain their choice owner's keys.
Thus `Pair(Z, A)` used as `Pair(Int, Bool)` has Z = Int and A = Bool, regardless
of caller parameter names. Each closed data layout is emitted once across
requesters, and only its declaring module retains its data export/exposure.
Opaque exports still have full Core layouts but cannot be constructed or
projected by source importers; passing them through a third module adds no export.
Generic function code is instead deduplicated by (requesting module, defining
declaration, concrete type list). Thus Main and Other can own distinct copies
`Main::Lib.identity(Int)` and `Other::Lib.identity(Int)`. Nested requests use
the instance's requesting module. Full names retain origin/type arguments,
declaration metadata keeps the defining span, and ModuleMeta retains requester
ownership. Overloaded functions add `@Receiver` before the type list. Duplicate
code across modules is accepted in Phase 2; later sharing must retain meaning.
Concrete types and function instances use bounded hash indices, source
function lookup uses a sorted index, and module metadata visits each owned
instance directly. Concrete source types share a cache across substitutions.
Index buckets and source-function ordering use names, declaration positions,
program indices and canonical type hashes, never addresses; ASLR cannot change
lowering's work-limit verdict. Native scalar leaves share cache entries across bodies.
Instances bind type-only requirement uses by (callee's scoped parameter, ability),
not by concrete type. Each binding retains its selected proof and the supplying
instance's type/proof environment. PARAMETER replacement follows these bindings
iteratively under the ability-path/work bounds. A composite proof for `List(U)`
passed from `relay(U)` to `show(T)` retains relay's substitution for its parts,
not show's unrelated T. FUNCTION parts use `part_parameters` to bind the
customizer's own requirements; receiver parameter order need not be alphabetical.
Imported calls use those same callee-owned keys and caller-supplied proof
environments; sharing structurally equal actual-type proofs does not merge the
keys or require pointer-equal actual types. Requesting ownership and defining
file provenance are separate: `Main::Lib.offset(Int)` and
`Bridge::Lib.offset(Int)` have different owners but retain Lib's declaration/body
spans. Generated derived helpers retain their responsible use span, not a
fictional source declaration. Only defining-module instances can appear in that
module's exposed-function list; importing requester copies gain no export.
Recursive derived graphs reserve helpers before walking parts. Missing keys
refuse E1150, including Number/Equal operations that could otherwise be guessed
from their substituted type. Native Order proofs preserve direct scalar
comparisons, notably IEEE Float operators rather than total `compare` ordering.
The C lowerer checks structural Core type height after substitution as well as
source-template depth. `T` becoming a deep list can push `List(T)` over Core's
path bound; that instantiation refuses with E1153 rather than returning invalid Core.
The E1152 document bound counts canonical types and every unfolded expression
occurrence across all function bodies, not just allocated expressions per body.
The canonical printer has separate schema/metadata depth, node, byte and scratch
bounds. Lowering does not exercise that codec: a compilable Core document need
not fit its text form. Callers requesting text handle the printer's refusal.

### 9.2 The five abilities

Each use of an ability becomes the specific primitive or function for the
type it is used at:

| Ability | In Core |
| --- | --- |
| `Equal` (`==`, `!=`) | `%eq` at the type: structural equality, maps by their entries and sets by their elements, in any order, floats by IEEE, ranges by their ends. `a != b` is `%not(%eq(a, b))`. |
| `Hash` | nothing to call: map and set primitives compare keys and elements with the same structural equality, so a map or a set can be a key or a set member, and equal ones, in any order, are the same key. Floats have none, so they are never keys. |
| `Order` | for `Int`, `BigInt`, `Byte`, `Decimal`, `Float`, `Bool` (`false` first), `Char` and `Text` (by character code), `Bytes` (byte by byte, a sequence before any longer one it begins), `Time` and `Duration`, the primitives `%cmp.b.lt`, `le`, `gt`, `ge`, which follow IEEE for `Float`, as the bytecode's `lt` and `le` do (`bytecode.md` 4.4). For a tuple, a list, a record or a choice type, a Core function `compare[T]`: the module's own `compare`, or one the lowering derives (tuples part by part from the left; lists element by element, a list before a longer one it starts; records field by field and choice types case by case, in declaration order, so `None` before `Some`, `Ok` before `Error`, and `Less`, `Equal`, `Greater`). `Unit` has one value, which never comes before itself: `a < b` is `false` once both sides are computed. `sort`, `min`, `max`, `sort_by` and `sort_by_desc` are Core functions instantiated with it (`Lib.lt`), in which a `Float` that is "not a number" comes after every other float, as in the bytecode's `cmp_float`. Maps, sets and ranges have no order (reference 9.2). |
| `Text` | the type's text form, as in 3.4, for `"#{x}"` and `x.to_text` alike. |
| `Number` | the arithmetic primitives of the type; `sum` is the Core function `List.sum[T]`. |

Derived record/choice text uses one-level constructor cases. Its proof parts are
flattened in case/field declaration order, including zero parts for an empty case.
Each field converts once in declaration order; argument spelling order only
controls the earlier construction. Empty constructors print their bare names,
others use `Name(field: value, ...)`, omitting labels for positional fields.
Nested native Text and Char use source quoting, unlike top-level `to_text`.
List Text uses a join over the remaining list and a reversed List(Text) of
converted pieces. Each head converts once before advancing the tail. At
exhaustion, reverse the pieces, `%text.join` with `, `, and surround with `[`/`]`.
Nested Text/Char elements are quoted; an empty list prints `[]`.

Derived tuple Order emits one Ordering case per part: Less and Greater return
immediately, and Equal alone compares the next part. Selected custom Order
functions follow the same protocol. `<` and `>` test the returned constructor;
`<=` is not Greater and `>=` is not Less. Native Float `compare` tests NaNs
before its numeric comparisons: two NaNs give Equal, a NaN sorts after any
number. This does not change direct scalar Float comparisons, which stay IEEE.
Derived records and choices use exhaustive one-level cases on both operands.
Different constructors order by declaration position without comparing fields;
matching constructors compare fields using the same flattened proof offsets as
derived Text. All generated cases and their field slots charge the lowerer's
whole-program work budget; there is no recursive C proof or value walk.
List Order emits a recursive join with the two remaining lists as parameters.
One-level `uncons` cases decide exhaustion; matching heads compare once and an
Equal result jumps with both tails. A proper prefix sorts before its extension.

A derived order, for `Point` of 4.1:

```
fn compare[Point](a: Point, b: Point) : Ordering =
  case a of Point(ax, ay) -> case b of Point(bx, by) ->
    if %cmp.decimal.lt(ax, bx) then Less else if %cmp.decimal.gt(ax, bx) then Greater else
    if %cmp.decimal.lt(ay, by) then Less else if %cmp.decimal.gt(ay, by) then Greater else Equal
  end end
```

and `a < b` at `Point` is `%eq(compare[Point](a, b), Less)`. From
`inventory.mo`:

```mo
fn top_by(items: List(T), n: Int, key: fn(T) : K) : List(T) where K: Order
  items.sort_by_desc(key).take(n)
end
```

used with products and `stock_value`, is

```
fn top_by[Product,Decimal](items: List(Product), n: Int, key: fn(Product) : Decimal) : List(Product) =
  %list.take(List.sort_by_desc[Product,Decimal](items, key), n)
```

where `List.sort_by_desc[Product,Decimal]` compares keys with
`%cmp.decimal.lt`.

## 10. Modules

### 10.1 to 10.3 The header, hidden insides, use

Core is one whole program, with every name resolved to a declaration.
Functions retain their owning module as metadata. `use` is resolved and
leaves no semantic operation. `expose` and hidden insides likewise have no
semantic operation, but **remain in ModuleMeta**: complete owned declaration
lists and exposed functions/types with `contents_exposed` (`T` false,
`T(..)` true). All types keep their full definitions for layouts/structural
printing. The image's module table and `mo test`'s hidden-type generation
use exposure, never infer it from those definitions. Generic function instance
ownership is the requesting module, as architecture 4.2 requires; canonical
generic data belongs to its defining module. Constants' module/source
order is retained separately from their dependency-ordered declarations.

### 10.4 Constants

A constant is `ConstDecl`: `const name : T = e`. A use is `global name`,
not a function call. The checked program already orders declarations by their
dependencies, including dependencies through called functions; lowering retains
that order (reference 10.4; bytecode 3.2 breaks ties by module then source order).
`initConsts` evaluates each once with an empty local environment and only
the already computed globals, appends its value, then continues. A stop
ends initialization before the entry or a test; no continuation runs.

The checker types each initializer in ordinary mode at its prefix index.
Every function declares `constCount`, the required prefix, and its body is
checked at exactly that prefix. Calls require the callee's prefix to fit
the caller's, so dependencies are checked transitively, including recursive
calls. An undersized or out-of-bounds prefix is rejected. Lowering may conservatively
overestimate it; `wholeProgram` gives the samples' functions the full prefix
because their initializers don't call them. Closures capture the computed
prefix where they are made. Constants' names must be distinct, and their
namespace is separate from local bindings; local shadowing cannot alter a
resolved global read. Cycles and forward reads, direct or through calls,
cannot pass these rules.
The C lowerer starts ordinary functions at the full prefix and tightens a
callee to the earliest constant initializer that can reach it. A bounded
work queue propagates those restrictions through direct calls and generated
function-value adapters, including calls nested in closures. Initializers are
ConstDecl bodies, not duplicate ordinary functions; codegen emits their `.init`
functions. Constant metadata carries source-order ownership and full body spans
independently of the dependency-ordered Core declarations.
Calls through constant function values translate to `app (global name) args`;
only leaf reads translate to `global name`. Core lambdas retain the prefix at
creation, even when their bodies are not executed during initialization. The
same rule applies to certified foreign reads and direct imported initializer
calls/function values: the checked declaring symbol supplies `Lib.seed`, not an
importer-local `Main.seed`. Initializer ownership, body spans and lifted closure
names retain that declaring source independently of dependency order; no
importer export is synthesized. Owned generic nominal/Text values cross this
boundary without changing layouts or capture carriers. Unresolved higher-order,
returned or anonymous initializer calls and executed abstract Text/Order
dependencies still refuse checker E1103 rather than emitting incomplete Core.
The current C lowerer refuses E1150 if an initializer's deferred lambda or named
function adapter needs a global outside that prefix. A bounded name index and
the existing call work queue detect this before returning Core. The checked
constant order remains unchanged: constructing a function does not acquire a
false execution dependency. Supporting these later-global closures needs a
separate representation decision, not a change to the producer's dependency walk.

```mo
max_attempts = 3
t0 = Time.parse("2026-10-12T09:00:00Z") or stop "the time is written correctly"
```

```
const max_attempts : Int = 3
const t0 : Time = case %time.parse("2026-10-12T09:00:00Z") of
                   None -> stop "the time is written correctly"
                 | Some(%or) -> %or
                 end
```

### 10.5 Always in scope

`Option`, `Result`, `Ordering`, `FileError`, `Entry` and `EntryKind` are built
into Core's data table (`ctorsOf`); the capability types are Core types;
the section 14 functions are primitives or library functions. `Entry` has
one constructor, `Entry(name: Text, kind: EntryKind)`. `EntryKind` has
`File`, `Folder`, `Link`, `Other`, in that order, each without fields.
These are distinct built-in types, not user declarations named `Entry`.

## 11. Capabilities and effects

### 11.1 What a function can touch

A capability is a value of type `cap c`: a handle, `cap c root`, where `root`
is the folder a `Files` or `ReadFiles` handle is narrowed to, or nothing for a
handle that reaches nothing (11.3). Every effect is a primitive on a handle
(`%io.print`, `%io.readText`, `%io.now`, ...), which reads and updates the
one `World`: arguments, environment variables, standard input, standard
output and error, the files, the clock's time and the four-word random state. A function
without a capability parameter can't reach them.

The world's files are texts, as `Files.in_memory` makes them (reference 14),
so `files.read_bytes(path)`, which is `%io.readBytes(files, path)` on `Files`
and on `ReadFiles`, gives the UTF-8 encoding of the file's text: the bytes a
real file holding that text holds. Its failures are `read_text`'s: `NotFound`
for a file that isn't there, and `NotAllowed` for a path that would leave the
folder (11.3), or `Failed(path, "is a folder")` for a folder and
`Failed(path, "not a folder")` when traversing a file (D236 below).
A real file can hold bytes that aren't UTF-8, which
`read_text` can't give as text (`NotText`, reference 14) and `read_bytes`
can; no file in the model is like that.

### 11.2 main

`main` lowers like any function. The runtime makes one handle for each
capability `main` declares, evaluates the constants (10.4), calls `main`,
and turns its result into the exit status:

| `main` returns | Exit status |
| --- | --- |
| nothing (`Unit`) | 0 |
| `Int` | that `Int`; outside 0 to 255 it is a bug |
| `Result(Unit, E)` | 0 for `Ok`; for `Error(e)`, `e`'s text form on standard error, then 1 |

In Core this is a wrapper. Its parameters are main's capabilities in written
order. A Result entry reuses main's Console when present; otherwise it appends
one generated Console from the runtime, used only for printing Error's text.
That additional capability is never passed to source main. Unit/Int wrappers
have exactly main's declared capabilities, with no generated Console. The host
constructs exactly the wrapper's declared capabilities through `mo_call_main`:

```
fn %start(args: Args, console: Console, files: Files) : Int =
  case main(args, console, files) of
    Ok(%_) -> 0
  | Error(e) -> let %_ = %io.printError(console, TodoError.to_text(e)) in 1
  end
```

### 11.3 Passing less

`files.within("reports")` is `%io.within(files, "reports")`, a handle on the
folder `reports` inside the handle's own; `files.read_only` is
`%io.readOnly(files)`, a `ReadFiles` handle on the same folder.

A path is relative and uses `/`. Its `.` parts are dropped and a `..` takes
back the part before it, so `a/../b` is `b`. Given a path that would leave
the handle's folder, such as `..` or `../b`, reading, writing, removing and
listing give `Error(NotAllowed(path))`, and `exists?` gives `false`
(reference 14). The model treats a path with an empty part (`""`, `/a`,
`a//b`, `a/`) the same way, as not relative. `within` a path that would leave
the folder gives a handle that reaches nothing (reference 11.3, 14),
`cap c none`: every call on it that returns a `Result` gives
`Error(NotAllowed(path))`, `exists?` gives `false`, and its `within` and
`read_only` reach nothing too.

### 11.4 Rules

Anonymous functions' capability-capture restriction remains a front-end
check. Core now checks the spec boundary: `fail`, `%io.printed` and
`%io.printedErrors` require spec mode, and ordinary code cannot call a spec
function. `test`, `example`, `promise` and `anchor` bodies are spec code;
anonymous bodies inherit their enclosing mode. A named function used as a
value lowers to a lambda calling it (5.6), so this also rejects making a
wrapper for a spec function in ordinary code. Fake constructors will use
this same boundary when they are added.

## 12. Examples, promises and tests

### 12.1 Examples

Each line of an `examples` block is a zero-argument function of kind
`example`, returning `Unit` on success or failing with a Text detail.
Equality is `%eq` at the compared type. Lowering computes operands once and
supplies their structural text forms as failure details, converting only on
failure through the borrowed `Text.%structural`, never user `to_text`.
Equal-only values need no Text proof. A failed `is` reports the same structural
form for its snapshotted input. These forms agree with the test runner's JSON
`left`, `right` and `input`. Core 0.8 supplies `(prim (structural T) (value))`.
C lowering encodes each field as `LABEL N:VALUE,`, where LABEL is `left`,
`right` or `input`, N is the canonical decimal UTF-8 byte count, and VALUE is
the raw structural text. Equality concatenates left then right; `is` emits
input alone. The runner splits by lengths, so embedded labels, commas, newlines
and NUL cannot masquerade as fields. It displays the decoded values or puts
them into the corresponding JSON fields. No quoting changes the observation.
The structural result is bound once in the failure branch before taking its
byte count and adding its payload. Pattern failure joins live inside the saved
input's scope, and all one-level failure cases jump to that one join.

Parentheses and expression statements forward the diagnostic condition.
`and` reports its first false leaf; `or` reports its right false leaf only
after the left is false; `implies` reports its right false leaf only after the
left is true. A false left operand of `or`/`implies` must not fail prematurely.
Other false Bool leaves keep the fixed assertion/example detail. Successful
conditions never run structural conversion, and successful `assert ... is`
bindings remain in scope for later statements. Ordinary assertions still stop.

```mo
count("one two\nthree\n") == Counts(lines: 2, words: 3, bytes: 14)
```

```
example count.1() : Unit =
  let left = count("one two\nthree\n")
  let right = Counts(2, 3, 14)
  if %eq(left, right) then Unit
  else
    let l = %structural[Counts](left)
    let r = %structural[Counts](right)
    fail %text.concat(field("left", l), field("right", r))
```

Here `field(label, text)` abbreviates
`label ++ " " ++ %int.toText(%text.byteCount(text)) ++ ":" ++ text ++ ","`;
it is concatenation of already-computed native text, never source `to_text`.

### 12.2 Promises

A promise's body becomes a function of kind `promise`, taking the parameters
and a final result parameter, returning `Int`: 0 when every line holds,
otherwise the one-based number of the first false line. `promiseBody` lowers
the lines to nested `if`s. An anchor has kind `anchor`, no parameters and an
`Int` result: it builds its values and calls that promise. An `allows`
anchor must return 0, a `forbids` anchor a nonzero number. `mo test` supplies
generated inputs and the ordinary function's actual result; generation is
outside this model. `promiseHolds` compares an answer with 0 for the samples'
Boolean conditions.

C lowering emits each checked condition exactly once, nested under the previous
line's success branch. Promise names are `SUBJECT.promise.N`; anchors use that
whole name followed by `.allows.N` or `.forbids.N`, so distinct promises cannot
reuse a display name. Metadata preserves the ordinary subject/promise links,
expectation, verbatim promise header and each original condition's bytes/span.
Anchor value roots are evaluated by increasing checked postorder index (written
order), saved once, and only then permuted to parameter order at the promise call.

Each concrete subject instance reserves its promises with the same scoped type
and ability environment. An anchor's `arguments` and type-only `abilities`
(operand `UINT32_MAX`, keyed by subject parameter and ability) select that subject
instance; no fresh promise parameter scope or inferred diagnostic Text proof is
introduced. A promise is memoized by source spec plus concrete subject instance,
including the requesting module, before its body enters the bounded work queue.
Its name uses the concrete subject's full instance name before `.promise.N`.
Uses discovered in examples/tests and subsequent promise/customizer bodies also
reserve the corresponding promises. Unused polymorphic templates emit no Core
declaration. A generic subject's zero-parameter examples emit once, under its
uninstantiated overload-qualified name, regardless of the body's concrete calls.
Module ownership lists group all ordinary/spec instances by original source
declaration order; call/link indices stay fixed. Tools execute anchors separately
to check their expectations; typing and lowering alone do not establish truth.

```mo
promise "a deposit adds exactly the amount"
  result is Ok(after) implies after.balance == account.balance + amount
  allows account: alice(10.00), amount: 5.50, result: Ok(alice(15.50))
  forbids account: alice(10.00), amount: 5.50, result: Ok(alice(10.00))
end
```

```
promise deposit.promise.1(account: Account, amount: Decimal, result: Result(Account, AccountError)) : Int =
  if (case result of
    Ok(after) -> %eq(<after.balance>, %dec.add(<account.balance>, amount))
  | Error(%_) -> true
  end) then 0 else 1

anchor deposit.allows.1() : Int = deposit.promise.1(alice(10.00), 5.50, Ok(alice(15.50)))
anchor deposit.forbids.1() : Int = deposit.promise.1(alice(10.00), 5.50, Ok(alice(10.00)))
```

### 12.3 Tests

A test is a zero-argument Unit function of kind `test`; `assert e` is
`if e then <rest> else fail "assertion failed: ..."`, and
`assert e is Pattern` is a `case` that holds the rest of the test in the
matching arm and fails in the others. The test passes when it returns Unit.
`fail e` first evaluates `e : Text`; effects survive in the failed outcome's
world, and no continuation runs. A bug while computing the detail remains a
stop, not a failure. The sample helper `passes` lowers its Boolean condition
to a Unit assertion in spec mode.

C lowering names tests `MODULE.test.N` and examples
`SUBJECT.example.N` from the checked source ordinals, not traversal order.
Metadata retains decoded test wording, original example line bytes and the
source span. Ordinary false conditions produce `assertion failed` or
`example failed`; ordinary-function assertions remain stops. Equality/pattern
details follow the structural-only rule above when Core supports it.

### 12.4 Fakes

Fakes are Core primitive constructors, not the test's starting world:

| Mo | Core | Result |
| --- | --- | --- |
| `Console.fake()` | `%io.consoleFake()` | Console |
| `Console.fake_input(lines)` | `%io.consoleFakeInput(lines)` | Console |
| `Files.in_memory(files)` | `%io.filesInMemory(files)` | Files |
| `Clock.fixed(time)` | `%io.clockFixed(time)` | Clock |
| `Random.seeded(n)` | `%io.randomSeeded(n)` | Random |
| `Args.of(list)` | `%io.argsOf(list)` | Args |
| `Env.of(map)` | `%io.envOf(map)` | Env |

Each successful construction appends its own `CapState` to `World.fakes` and returns `cap kind root
(some index)`. A real handle has `none` for its state index and uses the
world's real fields as before. The index and folder root are independent:
`within` and `readOnly` preserve the index, so a root, narrowed handle and
read-only handle share every write. Two constructions of the same kind
receive different indices, so both may be used in one expression. Calls and
captured environments carry handles, not copies of state; an ordinary
`main` can write to a test's files and console, and the test sees that state
after the call. Inputs and observations are ordinary immutable values:
retaining the original map, or a previous `printed` list, keeps a snapshot.

The typing rule and checker permit all seven constructors, `printed` and
`printedErrors` only in spec code, including anchors and anonymous functions
inside spec code (bytecode 3.3 and 9), as for `fail`. Constants and ordinary
functions cannot allocate fakes. A real console observation stops with
`outside "console observation needs a fake"`; it never reads real output.
The kind type does not record whether a handle is fake, so this is a runtime
check, not a fake-vs-real typing distinction.

The world's standard input is a list of lines, each without its line ending,
which `%io.readLine` returns in turn. The runtime splits what it reads where
`\n` or `\r\n` ends a line, and turns each sequence of bytes that isn't valid
UTF-8 into one U+FFFD, the replacement character (reference 14);
`Console.fake_input(lines)` gives the lines themselves.

In `Files.in_memory`, folders start with every ancestor of a file and persist
after its removal; `make_folder` adds folders explicitly (D237 below). The
folder the fake stands for, `"."` of its original handle, always exists; `list` gives
the names directly inside a folder, each subfolder once, sorted, and listing
a folder that doesn't exist is `Error(NotFound(folder))` (reference 14).
`entries` has the same paths, errors and ordering, with `Entry` values:
stored subfolders have kind `Folder`, other names have kind `File`.
Removing the last descendant keeps its folders. `within` and
`read_only` retain the shared store, and earlier lists are snapshots.

Todo's and Find's multi-call tests are now single Core expressions that
construct fresh Args/Console handles as their Mo source does. No host step
replaces world fields between calls; the old `scenario` workaround is gone.

`wc.mo`'s test:

```mo
console = Console.fake()
files = Files.in_memory({"a.txt": "one two\n", "b.txt": "three\n"}).read_only
status = main(Args.of(["a.txt", "gone.txt", "b.txt"]), console, files)
assert status == 1
assert console.printed == ["      1       2       8 a.txt", "      1       1       6 b.txt", "      2       3      14 total"]
assert console.printed_errors == ["wc: gone.txt: no such file"]
```

lowers to the following expression in an empty real world:

```
let console = %io.consoleFake() in
let files = %io.readOnly(%io.filesInMemory({"a.txt": "one two\n", "b.txt": "three\n"})) in
let status = main(%io.argsOf(["a.txt", "gone.txt", "b.txt"]), console, files) in
if %eq(status, 1) then
  if %eq(%io.printed(console), ["      1       2       8 a.txt", ...]) then
    %eq(%io.printedErrors(console), ["wc: gone.txt: no such file"])
  else false
else false
```

## 13. Running

### 13.1 Evaluation

Core evaluates strictly and left to right: a `let`'s bound expression before
its body, the arguments of a call, a primitive or a constructor from left to
right, then the call. `and`, `or`, `implies` and the Option `or` (reference
13.1) lower to `if` and `case`, which evaluate only the branch taken. The
lowering keeps Mo's order wherever it reorders anything (record fields named
out of order, 4.1; `try` inside an expression, 8.3). A map literal computes
its keys and values in source order, then checks its keys (3.8).

### 13.2 Bugs

A bug ends evaluation with `stopped`, its reason, and the world so far:

| Reference 13.2 | `Bug` |
| --- | --- |
| overflow in `Int`, `Decimal` or `Duration` arithmetic; a time outside the years 1 to 9999 | `overflow what` |
| dividing by zero | `divByZero` |
| reaching a `stop` | `stop reason` |
| calling a library function outside what it accepts (`take(-1)`, `random.int(5, 5)`, `split("")`, `repeat(-1)`, `replace("", new)`, `round(19)`, ...) | `outside call` |
| two keys of a map literal that turn out equal as the program runs (reference 3.8) | `repeatedKey` |
| `Files.in_memory` makes one path both file and folder (D236) | `fileFolderConflict path` |

Running out of stack, running out of memory, an exit status outside 0 to 255,
and printing to an output that is gone are the runtime's, outside this model.
A computation that doesn't end runs out of fuel (`outOfFuel`).

### 13.3 Costs

Core says what a program computes, not what it costs. The library's
`sort_by` here is an insertion sort, and lists are Lean lists; the VM keeps
the costs of reference 13.3.

## 14. The core library

Functions marked *prim* are primitives (`MoCore/Prim.lean`, typed by
`Prim.sig`); *Core* functions are written in Core (`MoCore/Lib.lean`),
instantiated per type; *lowered* ones are a few lines of Core written in
place; *not modeled* ones are not in this model yet.

The C lowerer emits `prim (int (abs|min|max))` for the inventoried `Int.abs`,
`Int.min` and `Int.max`, whether selected by a namespace call or receiver call.
A first-class reference uses the existing typed lambda adapter with that same
primitive body; a constant alias is an application of the stored function value.
Namespace calls skip their pure callee row for every argument, not only the
first; receiver calls start with the receiver. Both evaluate arguments once in
written order. Int's minimum passed to `abs` raises the existing Int overflow.
No new Core representation or producer dependency is introduced; adapters retain
their source metadata and creation-prefix limits. This C subset does not imply
that every library primitive in the model's table below is source-lowered yet.

| Mo | In Core |
| --- | --- |
| `Int`: `+ - * / %`, unary `-`, `abs`, `min`, `max`, `pow`, `floor_div`, `floor_mod`, `checked_*`, `wrapping_*`, `saturating_*`, `to_decimal`, `to_float`, `to_big`, `to_char`, `to_byte`, `to_text`, `seconds` ... `days` | prim (`%int.*`) |
| `Byte`: `to_int`, `to_text` | prim (`%byte.*`) |
| `BigInt`: `+ - * / %`, unary `-`, `abs`, `to_int`, `to_text` | prim (`%big.*`) |
| `BigInt`: `min`, `max`, `pow`, `floor_div`, `floor_mod` | prim (`%big.min`, `%big.max`, `%big.pow`, `%big.floorDiv`, `%big.floorMod`) |
| `Decimal`: `+ - * / %`, unary `-`, `abs`, `min`, `max`, `round`, `floor`, `ceil`, `to_int`, `to_float`, `to_fixed`, `to_text` | prim (`%dec.*`) |
| `Float`: `+ - * / %`, unary `-`, `abs`, `floor`, `ceil`, `round`, `sqrt`, `nan?`, `infinite?`, `to_text` | prim (`%float.*`) |
| `Float`: `to_decimal` | prim (`%float.toDecimal`) |
| `<`, `<=`, `>`, `>=` at a built-in type | prim (`%cmp.*`) |
| `==`, `!=` | prim (`%eq`), with `%not` |
| `Text`: `byte_count`, `char_count`, `empty?`, `chars`, `bytes`, `lines`, `words`, `split`, `trim`, `trim_start`, `trim_end`, `starts_with?`, `ends_with?`, `contains?`, `drop_prefix`, `drop_suffix`, `replace`, `repeat`, `pad_left`, `pad_right`, `ascii_lowercase`, `ascii_uppercase`, `to_int`, `to_decimal`, `Text.from_chars`, interpolation | prim (`%text.*`) |
| `Text`: `to_float` | prim (`%text.toFloat`) |
| `Char`: `code`, `ascii_digit?`, `ascii_letter?`, `ascii_white_space?`, `ascii_lowercase`, `ascii_uppercase`, `to_text` | prim (`%char.*`) |
| `Bytes`: `size`, `get`, `decode_utf8` | prim (`%bytes.*`) |
| `List`: literals, `size`, `empty?`, `get`, `first`, `last`, `append`, `prepend`, `concat`, `reverse`, `take`, `drop`, `set`, `remove_at`, `enumerate`, `zip`, `contains?`, `unique` | prim (`%list.*`) |
| `List`: `join` | prim (`%text.join`) |
| `List`: `to_map` | prim (`%map.ofList`): for a key given twice, the later value, in the key's first place |
| `List`: `to_set` | prim (`%set.ofList`): each element in its first place, as `unique` gives them |
| `List`: `map`, `filter`, `filter_map`, `find`, `any?`, `all?`, `count`, `reduce`, `max`, `sum`, `sort_by`, `sort_by_desc` | Core (`List.map[A,B]`, ...); `max` gives the first of equal elements |
| `List`: `sort`, `min` | lowered: `sort` is `sort_by` with the element itself as key, `min` is `max` with the order reversed, so it too gives the first of equal elements (not written out in `Lib.lean`) |
| `List`: `flat_map`, `group_by` | lowered the same way as `map` (a loop with join points), not written out in `Lib.lean` |
| `Map`: literals (`%map.lit n`, which stops on a repeated key, 3.8), `size`, `empty?`, `get`, `has?`, `set`, `remove`, `keys`, `values`, `entries` | prim (`%map.*`) |
| `Map`: `update` | Core (`Map.update[K,V]`) |
| `Set`: `Set.empty`, `size`, `empty?`, `has?`, `add`, `remove`, `union`, `intersection`, `difference`, `to_list`, `for` | prim (`%set.*`); `for` is a list loop over `%set.toList` (3.8, 7.5) |
| `Range`: `a..b`, `for`, `to_list`, `contains?`, `to_text` | `%range.make`; a counting loop over `%range.start` and `%range.end` (7.5); `%range.toList`; `%range.contains`; `%range.toText` |
| `Option`: `or`, `or_fail`, `map`; `Result`: `map`, `map_error` | lowered: a `case` |
| `Time`: `+ -` with durations, `-` of two times, `checked_add`, `to_text`, `Time.parse`; `Duration`: `+ - *`, `in_milliseconds`, `in_seconds`, `to_text` | prim (`%time.*`) |
| `Args.list`, `Env.get`, `Console.print`, `error`, `read_line`, `printed`, `printed_errors`, `Files.read_text`, `read_bytes`, `write_text`, `append_text`, `remove`, `make_folder`, `list`, `entries`, `exists?`, `within`, `read_only`, `Clock.now`, `Random.int` | prim (`%io.*`); `make_folder` is `%io.makeFolder`, Files only; `ReadFiles` has the read-only forms; `read_bytes` gives a file's text as UTF-8 (11.1); Random uses xoshiro256**/SplitMix64/Lemire |
| `Files.walk`, `ReadFiles.walk` | Mo library code in `lib/`, not a Core primitive; lowering supplies recursive calls to `entries` |
| `FileError`'s text form | Core (`FileError.to_text`) |
| `to_text`, at every type with `Text` | lowered as the part `#{x}` of a text is (3.4), so the two give the same text; a set's is `Lib.setText` (3.4), and the `to_text[T]` the lowering derives for records, choice types, tuples, lists and maps without their own `to_text` is not modeled |
| `Console.fake`, `fake_input`, `Files.in_memory`, `Clock.fixed`, `Random.seeded`, `Args.of`, `Env.of` | spec-only primitives (`%io.consoleFake`, `consoleFakeInput`, `filesInMemory`, `clockFixed`, `randomSeeded`, `argsOf`, `envOf`), with independent shared states (12.4) |
| `Unicode16.graphemes`, `display_width`, `normalize_nfc`, `lowercase`, `uppercase`, `letter?`, `digit?` | trusted prim (`%unicode16.graphemes`, `displayWidth`, `normalizeNfc`, `lowercase`, `uppercase`, `letter`, `digit`), with the signatures below; reached calls stop in the executable oracle |

## Ambiguities settled by D227

The first version of this document listed 23 places where the reference left
a choice open. Robert settled them on 12 October 2026 (D227), and the
reference now gives every answer. For each: the answer, the reference section
that gives it, and whether the model already did that or was changed. The
section numbers after the topic are the reference's; those after "Already",
"Changed" or "Unchanged" are this document's.

1. **A map literal with a repeated key** (3.8, 13.2, 14): an error when the
   compiler can see the keys are equal, a bug when they turn out equal as the
   program runs; `to_map` keeps the later value, in the key's first place.
   Changed: a literal is now `%map.lit n`, which stops with `repeatedKey`
   (3.8); `to_map` already did that.
2. **`x or default` and `or_fail`** (8.1, 13.1): `or` on an `Option` computes
   `default` only for `None`; `or_fail(e)` is a function, so `e` is always
   computed. Already did that.
3. **Order of named fields** (4.1): fields are computed in the order they are
   written. Already did that.
4. **`try` and `return` in an anonymous function** (5.6): they leave that
   anonymous function. Already did that.
5. **`lines` and line endings** (14): `\n` and `\r\n` end a line, and a `\r`
   alone stays in its line. Changed: a lone `\r` at the very end of a text was
   dropped; now only a `\r` before a `\n` is.
6. **ASCII white space** (14): space, tab, line feed, vertical tab, form feed
   and carriage return. Already did that.
7. **`"".split(",")`** (14): `[""]`. Already did that.
8. **`Text.to_decimal`** (14): digits on both sides of a point, 1 to 18 after
   it, so `"1."` and `".5"` give `None`; `to_int` takes leading zeros and
   `-0`, as its letter allows. Already did that.
9. **Decimal `%`** (3.3): the remainder has the sign of the left side. Already
   did that.
10. **`round` and `to_fixed` places** (14): 0 to 18; anything else is a bug.
    Already did that.
11. **Other limits** (14): `repeat` takes a count of at least 0 and `replace` a
    non-empty text to replace, or the call is a bug; padding to a width the
    text already has or exceeds, negative ones included, changes nothing.
    Already did that.
12. **`FileError`'s text forms** (14): `PATH: no such file`,
    `PATH: not allowed`, `PATH: not text` and `PATH: REASON`. Already did
    that.
13. **Folders in `Files.in_memory`** (14): a folder exists when a file is
    inside it, and `list` gives the names directly inside, each subfolder
    once, sorted. Already did that; `exists?(".")` now also answers `true`
    for the fake's own folder, as `list(".")` already treated it (12.4).
14. **`within` a path that would leave the folder** (11.3, 14): a capability
    that reaches nothing, on which every call that returns a `Result` gives
    `Error(NotAllowed(path))` and `exists?` gives `false`. Changed: it was a
    bug; now it is a handle with no root (11.3).
15. **Times as text** (14): `to_text` writes UTC with `Z`, and the shortest
    fraction of a second only when there is one; `Time.parse` reads `T` or
    `t`, then `Z`, `z` or an offset it converts to UTC, and at most 6 fraction
    digits, and gives `None` for anything else, second 60 included. Already
    did that.
16. **Several fakes of one kind in a test** (12.4): each fake is a handle on
    its own state, and every use of one fake sees the same state. Now modeled:
    constructors allocate independent states and derived handles retain their
    index, including when several fakes of one kind are used together (12.4).
17. **When constants are computed** (10.4): once, before any other code of
    the program runs, in the order of their dependencies; a cycle is an
    error, and a constant that stops stops the program before it starts.
    Now modeled: ordered `ConstDecl`s, prefix checking and once-only
    initialization before entry or tests (10.4).
18. **`min` and `max` with equal elements** (14): the first of them. Already
    did that.
19. **`Float`'s text form** (9.2): numbers have their shortest spelling.
    Now modeled: `%float.toText` uses the same shortest decimal coefficient
    as `%float.toDecimal`, with the spelling choices below. Architecture
    4.5 still requires the runtime to be checked against Ryu independently.
    "Not a number" and the infinities retain D228's spellings.
20. **A `var` read by an anonymous function and assigned afterwards** (5.6):
    an error. Already consistent: a closure captures values when it is made,
    and the front end rejects the programs in which a later value could
    differ (5.6).
21. **`exists?` for a path that would leave the folder** (14): `false`, and
    reading, writing, removing and listing such a path give
    `Error(NotAllowed(path))`. Already did that; changed so that only a path
    that would leave counts: a `..` that stays inside the folder, as in
    `a/../b`, is now an ordinary path (11.3).
22. **`printed` on a real console** (12.4, 14): `printed` and
    `printed_errors` can be used only in tests, examples and promises, where
    every console is a fake. Now checked in Core too: observations require
    spec mode; anchors are included following bytecode 3.3 (11.4, 12.4).
23. **`Range` values** (9.2): `Equal` and `Hash`, no order, and the text form
    `0..3`. Equality and hashing already did that, on the pair of the ends;
    changed: the text form is now lowered to `0..3` rather than a tuple's
    (3.4, 7.5). A range has since become a value of its own type, still
    equal when its ends are, with the text form `%range.toText` (last
    section).

D227 settled a few more questions, and the model follows them too. `Option`'s
cases are declared and ordered `None`, `Some` (reference 8, 9.2): changed in
`ctorsOf`, and so in every `case` on an `Option` (section 0 here); nothing in
the model sorts or compares `Option` values by their order. A `Float` that is
"not a number" sorts after every other float, and `Unit` has an order
(reference 9.2's table): changed in `Lib.lt`, which `sort`, `sort_by`, `min`
and `max` use (section 9.2 here). Polymorphic recursion is an error
(reference 5.5, section 5.5 here). Standard input's line endings and invalid
UTF-8 are the runtime's (reference 14, section 12.4 here). The `mo` command's
exit status (reference 13.4) is outside Core.

## Additions in D228

D228 approved the reference as v0.1 with three additions, and the model
follows them. As above, the section numbers after the topic are the
reference's, and the others this document's.

1. **A time minus a duration** (3.9): `now - 1.hours` is a `Time`, a bug
   outside the years 1 to 9999 as adding is, and `checked_add` with a
   negative duration goes back in time. Already did that: `%time.subDur`
   (`TimeOp.subDur`), whose signature takes a `Time` and a `Duration` and
   gives a `Time`, has been in Core from the start, proven safe by
   `timeOp_safe` with the other time primitives (3.9). Nothing checked it
   before; `MoCore/Examples/RateLimit.lean` now does: a bucket refilled from
   `t0 - 1.hours`, the bug before the year 1, and `checked_add` with negative
   durations.
2. **`x.to_text`** (9.2, 14): the text form of any value whose type has
   `Text`, the same text as `"#{x}"`. It lowers to the same Core as the part
   `#{x}` of a text, and at a type whose text form the lowering derives, to a
   call of the same `to_text[T]`, so the two never differ (3.4, 14).
   Unchanged: the model doesn't generate `to_text[T]` for any type yet, so it
   has `x.to_text` where it has `#{x}`: at the built-in types, and at types
   with their own `to_text`, `FileError` included.
3. **`nan`, `inf` and `-inf`** (9.2): a `Float` that is not a number is
   written `nan`, and the infinities `inf` and `-inf`. Changed:
   `%float.toText` writes them so (`floatToText` in `MoCore/Prim.lean`);
   Lean's own `Float.toString` writes "not a number" as `NaN`.
   `MoCore/Lib.lean` checks all three. Any other float is still written as
   item 19 above says (3.4).

## `Byte`, `Bytes` and `Range` as types of their own

Architecture 4.1 asks the model for `Byte`, `Bytes` and `Range` as types of
their own, so that the VM, whose types `bytecode.md` 2.1 lists, can be tested
against it. The reference and the architecture leave the questions below
open, and the model decides them as each item says. As above, the section
numbers after the topic are the reference's, and those at the end of an item
this document's.

1. **A literal that is a `Byte`** (3.2, 3.3): a number literal takes the type
   its context needs, and the reference doesn't say whether that includes
   `Byte`. The model takes it to: with `b` a `Byte`, `b < 128` compares with
   the byte `128`. A byte literal is from 0 to 255; Core's typing refuses any
   other (`Lit.Ok`), and the front end reports it first. The bytecode's `byte`
   constants come from these literals (`bytecode.md` 3.1). (3.2)
2. **A byte's text form** (9.2): its decimal digits, as an `Int`'s: `200`.
   (3.2, 3.4)
3. **`get` on `Bytes`** (14): `None` for a position below 0 or past the end,
   as `List.get` gives. (3.4)
4. **What `decode_utf8` accepts** (3.4): well-formed UTF-8 as table 3-7 of
   the Unicode Standard defines it, so `None` for an overlong form, a
   surrogate, a code above U+10FFFF, a continuation byte out of place, a byte
   that never appears in UTF-8, or a sequence cut short. A byte order mark is
   the character U+FEFF, and is kept. `MoCore/Support/Bytes.lean` follows the
   table row by row, and `MoCore/Builtins.lean` checks it against Lean's own
   decoder. (3.4)
5. **The order of `Bytes`** (9.2, "byte by byte"): at the first place two
   sequences differ, the smaller byte comes first, and a sequence comes
   before any longer one it begins, as a list does (`[1, 2] < [1, 2, 0]`).
   Two texts' bytes are in the order of the texts themselves, by character
   code, as UTF-8 keeps that order. (3.4, 9.2)
6. **A `Bytes` literal** (3.4): Mo has no way to write bytes, nor a function
   that builds them from a list of bytes, so in Mo, bytes that aren't UTF-8
   can only come from `read_bytes` on a real file. Core has a literal for
   `Bytes`, as the bytecode has constants for them (`bytecode.md` 3.1). The
   lowering never makes one from Mo; tests of the model, and of the VM against
   it, use it to build bytes that aren't UTF-8. (3.4)
7. **`read_bytes` in the model** (14): the world keeps files as texts, as
   `Files.in_memory` takes them, so `read_bytes` gives the UTF-8 encoding of
   the file's text, with `read_text`'s failures. (11.1)
8. **A range whose end isn't above its start** (7.5): it is empty. `to_list`
   gives `[]`, `contains?` is `false`, and a loop over it runs no round. Such
   ranges are equal only when their ends are, as equality is structural:
   `5..2 != 6..2`. (7.5)
9. **`contains?(n)`** (14): `true` when the start `<= n` and `n <` the end,
   for exactly the numbers a loop over the range counts. (7.5)
10. **Taking a range apart** (7.5): Core's `%range.start` and `%range.end`,
    which Mo doesn't have, do it as `%list.uncons` takes a list apart; they
    are the bytecode's `range_start` and `range_end` (`bytecode.md` 4.4). A
    `for` loop reads its range's end once, before the first round, and counts
    from the start while below it, so the counter is never increased past the
    end and never overflows (architecture 4.3). `a..b` computes `a`, then
    `b`, and building a range is never a bug. (7.5)
11. **A range's text form** (9.2): `0..3`, or `-2..10`, written by the
    primitive `%range.toText`, as every other built-in type with a text form
    has its own primitive, rather than by a concatenation the lowering
    writes out. (3.4)

## `Set` as a type of its own

Architecture 4.1 lists `Set` among the types the model lacks: it lowered
`Set(T)` to `Map(T, Unit)`, and the VM has a set type of its own
(`bytecode.md` 2.1). A set is now a value of type `set T`, its elements in
insertion order (3.8). Value typing requires that no element equals (`==`)
one after it, and every set primitive keeps that, so a set a program computes
never holds an element twice (`set_result_distinct`). The reference leaves
the questions below open, and the model decides them as each item says. As
above, the section numbers after the topic are the reference's, and those at
the end of an item this document's.

1. **`add` with an element already there** (3.8, 14): setting an existing key
   keeps its place, and so does adding an element a set holds: the set is
   left as it is. It keeps the element it held, not the new one, though the
   two are equal. That shows only where equal values differ in another way,
   such as two maps with the same entries added in different orders: with
   `m2` equal to `m`, `[m].to_set.add(m2)` still holds `m`, whose keys come in
   `m`'s order, as `%map.set` keeps the key it held. `List.to_set` and `union`
   add elements in the same way, so each keeps the first of equal elements,
   and `union` the first set's. (3.8)
2. **An element removed, then added again** (3.8): it goes last, as any new
   element does. Reference 3.8 visits keys "in the order they were first
   added", and a removal forgets an element, so it counts from when it came
   back: `[3, 1, 2].to_set.remove(3).add(3)` holds `1, 2, 3`. A map's key
   removed and set again goes last too. (3.8)
3. **`union`'s order** (14): the first set's elements in their order, then
   those of the second that the first doesn't hold, in the second's order, as
   if each were added to the first in turn: `[3, 1].to_set.union([2, 1,
   4].to_set)` holds `3, 1, 2, 4`. (3.8)
4. **`intersection` and `difference`** (14): the elements of the first set
   that the second holds, or doesn't, in the first set's order:
   `[3, 1, 2].to_set.intersection([2, 3, 5].to_set)` holds `3, 2`. (3.8)
5. **`List.to_set`** (3.8, 14): the elements in the order they first appear,
   each in its first place, as `unique` gives them: `[3, 1, 3, 2].to_set`
   holds `3, 1, 2`. (3.8)
6. **A set's text form** (9.2): its elements in insertion order, written as a
   list's, then `.to_set`: `[1, 2].to_set`. An element is written as inside
   any other value, so a text is quoted and a set inside a set is written as
   alone: `[[1].to_set].to_set`. The empty set is `[].to_set`, not
   `Set.empty`, so that every set is written in the one form that reference
   9.2 and `bytecode.md` 8 give. Two equal sets whose elements came in
   different orders are written differently, `[1, 2].to_set` and
   `[2, 1].to_set`, as two equal maps are. The text form is a Core function
   the lowering derives for each element type, `to_text[Set(T)]`, as it is
   for lists and maps, not a primitive: an element's text form may be the
   element type's own `to_text`, which a primitive can't call. (3.4)

## Failure and the program layer: decisions and mismatches

1. **Failure.** `fail e` fits any expected type, like `stop`, but requires
   spec mode and `e : Text`. It evaluates the detail strictly, retains the
   resulting world, and propagates unchanged through sequencing, calls and
   joins. The evaluator is unchecked, like the old evaluator; guarantees
   apply to expressions/programs accepted by typing or the checker.
2. **Function signatures.** The four spec kinds have bytecode 3.3's
   signatures. A promise must have at least its final result parameter and
   return Int; parameter names have no semantic role. Compiler metadata links
   a promise to its ordinary subject and an anchor to its promise, carrying
   the anchor expectation and each condition's text/span in source order.
   Lowering supplies matching input/result types and generated line-number
   code; metadata shape checks do not establish their correspondence.
   Ordinary `function` signatures remain unrestricted.
3. **Spec is lexical, not a function-type effect.** Lambdas inherit the
   body's mode. A spec caller may pass its lambda to ordinary higher-order
   library code, such as `List.all`; applying it executes its spec body.
   Ordinary code cannot name a spec target or construct a wrapper calling
   one. `Ty.fn` is unchanged: it carries argument and result types, not a
   spec flag, as bytecode 2.1 does. An arbitrary host-supplied closure can
   therefore execute spec code even when passed to an ordinary function;
   the claim that a runtime-created `main` cannot reach spec code relies on
   its capability-only arguments and ordinary initialization, not merely on
   value typing of arbitrary function arguments. Non-reachability is not
   separately proven here.
4. **Globals and dependencies.** Resolved globals use a separate namespace;
   declarations must have unique names. Lowering supplies the ordered list
   and each function's conservative `constCount`; Core checks rather than
   computes dependency order. Calls trim globals to that checked prefix;
   closures retain their creation prefix. Module names are metadata, not
   additional lookup keys. Handwritten samples put their function instances
   in their sample module; a front end can preserve each library module.
5. **Entry and runners.** The designated entry must exist and be ordinary.
   `runProgram` takes typed runtime argument values and returns its Core
   outcome; it does not manufacture capabilities, convert OS exit statuses,
   or synthesize the 11.2 wrapper. Samples with `main` designate it directly;
   libraries designate `transfer`, `request`, `lease` or `search_notes`.
   `runTest` accepts only a test or example and invokes it without arguments;
   misuse of either runner is `wrong`. The safety theorems require correct
   arguments or a valid selection.
6. **Initialization lifetime and fuel.** `runProgram`, `runTest` and
   `evalProgram` each initialize a fresh run. A suite or shared image uses
   `initConsts` once and its continuation receives reusable computed globals;
   a multi-call test keeps its handles in one expression. No image cache is
   hidden in World. Fuel is a depth
   bound reused for each initializer and the subsequent call, not a shared
   instruction budget. Larger fuel preserves every finished outcome, proven
   for initialization and both runners as well as expressions.
7. **Details in handwritten checks.** `passes` wraps its
   conditions in Unit assertions with fixed Text details; JobQueue's pattern
   assertions now fail rather than stop. Greet's examples are actual named
   example declarations. Generating operand text forms, all named test and
   anchor declarations, the test-suite driver and entry-wrapper generation
   belong to lowering, not this hand translation.

Two design/reference mismatches are left unchanged, as required:

- Architecture 4.1's introductory list of safe outcomes omits `failed`,
  though its syntax and architecture 8 include it. Core includes it.
- Reference 11.4, 12.4 and 14 list tests, examples and promises for fakes and
  console observations, but omit anchors. Architecture 4.1 and bytecode 3.3,
  7 and 9 include anchors as spec code; Core follows that definition.

There is also an entry-wrapper question: architecture 4.3 says its wrapper
receives the capabilities `main` declares, but an `Error(e)` must print to
standard error even when `main` declares no Console. Section 11.2 above
already chooses an extra runtime Console for that wrapper. This program
layer leaves wrapper construction to lowering; no new host capability is
silently added by `runProgram`.

## Fake capabilities: model decisions

1. **Identity and lifetime.** Each fake gets a zero-based, append-only index
   in `World.fakes`. Dropping all handles does not remove/reuse its state in
   this executable model; heap reclamation and reference counts are VM work.
   Folder roots and identity are independent. `CapState` uses the existing
   nine real-state fields (including D237 folders), with no kind tag or nested fake store; kind
   chooses the fields an operation uses. Source-created cross-kind aliases
   are only Files/ReadFiles. Host-supplied cross-kind aliases can select
   different fields of the same record, as TEXT.md specifies.
2. **Seeds.** `Random.seeded` takes Int, including negative numbers, interpreted
   modulo 2⁶⁴. It now initializes architecture 7's generator, precisely as
   specified below. Real and fake handles execute the same algorithm.
3. **Constructor inputs.** Args/input lists and file/environment maps keep
   their values and order. No input splitting or path normalization occurs
   on construction; the existing file rules resolve paths on use. Source map
   constructors and oracle host validation enforce distinct keys; `ValTy.map`
   alone does not. Retained maps and observations are snapshots.
4. **Malformed handles and observations.** Value typing covers both real
   and fake handles without a state-store invariant. A dangling fake index
   stops with `outside "invalid fake handle"`, even in an arbitrary host
   world, keeping unconditional type safety. `mocore` recursively rejects
   dangling host handles as `invalidRun`; lossless decoding preserves them
   for debugging. Real console observations stop with `outside "console
   observation needs a fake"`; capability types do not encode provenance.
5. **Interchange.** Core text is 0.8 only. Every handle explicitly carries
   real/fake provenance, and World transports the full store. Reports include
   all final fake states, including unreachable states and random words, in allocation
   order, alongside the existing real effects. Fuel exhaustion still has no
   world. Twelve real-entry exports remain controls; the Fakes spec export
   exercises constructor allocation, sharing, isolation and snapshots, and
   Library exercises the random vectors and folder entries. The sample
   translation and fixture are `JsonQuery`, matching `examples/json-query.mo`
   and `module JsonQuery`; its locally declared recursive type remains `Json`.

## Library surface: decisions and trusted boundaries

### Stable seeded Random

`Support/Random.lean` implements xoshiro256** 1.0. All words are unsigned
64-bit integers; addition, multiplication and left shift discard bits above
bit 63. Right shifts are logical and rotations are left rotations. Seeding
is **exactly**:

1. Set SplitMix's state `x` to the Int seed modulo 2⁶⁴ (so −1 becomes
   `0xffffffffffffffff`), not its absolute value or a byte encoding.
2. Repeat four times: increment `x` by `0x9e3779b97f4a7c15`; set
   `z = (x xor (x >> 30)) * 0xbf58476d1ce4e5b9`, then
   `z = (z xor (z >> 27)) * 0x94d049bb133111eb`; output `z xor (z >> 31)`.
3. Assign those outputs to `s0`, `s1`, `s2`, `s3` in order. The first word
   is produced **after** incrementing the seed. Do not retain SplitMix's
   final `x`, or reverse words according to machine endianness.

Each raw draw returns `rotl(s1 * 5, 7) * 9`. Then, in reference order,
`t = s1 << 17; s2 ^= s0; s3 ^= s1; s1 ^= s2; s0 ^= s3; s2 ^= t;
s3 = rotl(s3, 45)`. No jump or warm-up occurs.

`int(low, high)` requires `low < high`, with **exclusive high**. Calculate
`span = high - low` exactly, without signed 64-bit overflow. Thus spans
from 1 through 2⁶⁴−1 work, including `int(Int.min, Int.max)`; span 2⁶⁴
cannot be expressed by two Int endpoints. For each raw draw `r`, calculate
the full 128-bit product `m = r * span`, its low word `l`, and high word `h`.
Reject while `l < (2⁶⁴ - span) % span`; return `low + h`. Rejected draws
advance the same state and are never reused. Lean's Nat product retains
all 128 bits, including for spans above 2⁶³. This is Lemire's multiply-high
rejection, not remainder reduction or floating-point scaling.

To keep evaluation total without an arbitrary retry limit, rejection has
a bound of 2²⁵⁶−1 draws, the reference generator's nonzero-state period.
The published period and coverage guarantee an accepted draw; that
functional property is trusted, not proven here. An all-zero host state
stops with `outside "Random has an all-zero state"`; exhausting the bound
stops with `outside "Random exhausted its period"`. Neither state arises
from the specified seeding. Lossless text decoding preserves even the zero
state for debugging. Default real/fake records use the four words of seed
0; real-world entropy acquisition remains runtime work, not oracle I/O.

Fixed vectors in `Builtins.randomVectors` were obtained by compiling the
published [SplitMix64](https://prng.di.unimi.it/splitmix64.c) and
[xoshiro256**](https://prng.di.unimi.it/xoshiro256starstar.c) C reference
code. An adapter renames their `next` functions to `sm_next` and `xo_next`,
sets SplitMix's `x` to each seed, and assigns `s[i] = sm_next()` for i=0..3.
Only the xoshiro source prefix through `next`, before the jump routines, is
needed. Compile with `gcc -std=c17 -O2 -Wall -Wextra -Werror`. For seeds
0, 1, −1, Int.min and Int.max, guards compare all four seeded words and
the first six raw draws to independent fixed constants. For example seed 0
starts with `e220a8397b1dcdaf 6e789e6aa1b965f4 06c45d188009454f
f88bb8a8724c81ec`, and draws `99ec5f36cb75f2b4 bf6e1f784956452a
1a5f849d4933e6e0 6aa594f1262d2d2c bba5ad4a1f842e59 ffef8375d9ebcaca`.

Public `Random.seeded`/`Random.int` checks also compare signed intervals
and singleton intervals at both Int extremes. The independent unbatched
Lemire C sample in [Appendix A, arXiv:2408.06213v2](https://arxiv.org/html/2408.06213v2)
gives offsets `4800180567299270261, 5295190459760845450,
3609369285294772691, 3515805966490203214` for seed 1 and span 2⁶³+1,
consuming 2, 1, 1 and 4 raw draws. These discriminate rejection from biased
multiply-high without rejection. The maximum-span case checks signed
overflow and both high/low product halves. No generator correctness or
statistical randomness theorem is claimed; type/range safety stays proven.

### Unicode16's interface, not its tables

| Mo function | Core operation | Signature |
| --- | --- | --- |
| `graphemes` | `UnicodeOp.graphemes` | Text → List(Text) |
| `display_width` | `UnicodeOp.displayWidth` | Text → Int |
| `normalize_nfc` | `UnicodeOp.normalizeNfc` | Text → Text |
| `lowercase` | `UnicodeOp.lowercase` | Text → Text |
| `uppercase` | `UnicodeOp.uppercase` | Text → Text |
| `letter?` | `UnicodeOp.letter` | Char → Bool |
| `digit?` | `UnicodeOp.digit` | Char → Bool |

These are ordinary, pure `Prim.unicode16` operations, with checker support
and type safety. The evaluator remains executable: **when a call is reached**,
after evaluating its arguments, it stops with
`outside "Unicode16 is a trusted primitive unavailable in mocore"`
and retains the world so far. `mocore` emits that stopped report, exit 1;
a call in an unchosen branch or an unused function does not prevent
execution. This is an oracle boundary, not the runtime meaning of Unicode16.
Differential runs reaching it must be classified as unavailable, not used
to demand that the VM also stop.

Architecture 4 requires runtime conformance against UCD 16.0's own files,
not agreement with Lean: GraphemeBreakTest.txt, NormalizationTest.txt,
UnicodeData.txt, SpecialCasing.txt and the relevant width/property data.
No Unicode tables, ASCII fallback, UCD conformance claim, or extra logical
axiom is added here. A future runtime must specify display-width policy
where UCD data alone does not settle it; this interface does not choose one.
The existing ASCII Text/Char primitives are unchanged. `Int.to_char` already
rejects surrogates and code points above U+10FFFF; public guards now check
−1, both sides and both ends of the surrogate gap, U+10FFFF, U+110000 and
Int.max. No conversion fix was needed.

### Entries and the text contract

`IoOp.entries false/true` correspond to `FILES_ENTRIES`/`READFILES_ENTRIES`
in `src/base/hostops.def`, returning `Result(List(Entry), FileError)`.
`Entry`/`EntryKind` are D232's built-ins, usable in constructors, cases,
equality, host validation and structural reports without user declarations.
Entries contain immediate **names**, not paths or file contents; order is
the existing `list` order (Unicode scalar order, no normalization).

Both real handles in the modeled world and `Files.in_memory` use the same
text-file map and explicit folder list (D237 below); links/special files
cannot be represented by that state. `Link` and `Other` remain constructible
and transportable; classifying real OS links without following them belongs
to the C host implementation. Fake construction now rejects a file/folder
collision (D236 below). The top folder exists even when empty; stored empty
folders exist too, and missing folders give `NotFound`. No Core `walk` is added.

Text 0.3 adds these types/operations and replaces the old natural seed
with explicit `RandomState` words in World/CapState. It does not silently
upgrade 0.2 or reinterpret old state. Text 0.4 adds the numeric operations
below; 0.5 adds compiler metadata; 0.6 implements D236; 0.7 implements D237. Lean and C codecs
move together, with all 15 run fixtures and 14 fuzz seeds generated by `ExportSamples.lean`;
the freshness check compares both directories byte for byte. Version 0.8
refuses malformed fake file keys at construction. The next minor version is
0.9. TEXT.md specifies exact fields and boundary reports.

### Numeric completion in text 0.4

1. **BigInt.** `min`/`max` keep the first equal operand. `pow` takes an Int
   exponent, stops for a negative one, and defines 0⁰ = 1 as Int.pow does.
   `floorDiv` uses Lean's floor quotient, including negative divisors;
   `floorMod` is a − floorDiv(a,b) × b. Either stops on a zero divisor.
   Arithmetic remains unbounded, not narrowed to Int.
2. **One binary64 rounding.** `ratioToFloatBits` takes the exact magnitude
   ratio and sign, locates the exponent with integer arithmetic, then
   rounds the significand half to even. Subnormals round in 2⁻¹⁰⁷⁴ units;
   a carry can cross into normal values or infinity. Negative underflow
   keeps its sign. Decimal.to_float uses count/10¹⁸ directly, never
   Float.ofInt followed by division. No Float.ofScientific is used.
3. **Text.to_float.** The exact grammar is reference 14's optional minus,
   integer digits, optional point with digits and optional e/E exponent
   with optional sign. No surrounding space, separators, leading plus or
   case variants of special names are accepted. `nan` becomes canonical
   quiet NaN `0x7ff8000000000000`; `inf`/`-inf` are accepted explicitly.
   Numeric overflow that rounds to infinity gives None; signed underflow
   gives Some(±0). Large exponents are classified before constructing
   their powers of ten, after full syntax validation.
4. **Shortest decimal and Float.to_decimal.** Search 1 through 17
   significant digits for a decimal that the exact converter rounds back
   to the original bits. Select the nearest coefficient, ties even;
   adjacent candidates account for asymmetric power-of-two intervals.
   Float.to_decimal scales that exact decimal to 10⁻¹⁸ and rounds half to
   even once. Nonfinite values or a rounded count outside Decimal's range
   give None. This is deliberately not the exact binary-rational decimal:
   0.1 gives 0.1, not 0.100000000000000006.
5. **Float layout (superseded by D236).** Float.to_text renders the same
   coefficient and removes trailing zeros. D236 replaces the original
   shorter-of-fixed/scientific choice with the layout specified below;
   the shortest coefficient and Float.to_decimal are unchanged.
6. **Float remainder.** `%` truncates the exact quotient toward zero, like
   C fmod, not IEEE remainder's nearest quotient. The result and a zero
   keep the dividend's sign; a finite dividend with infinite divisor is
   unchanged. NaN operands, an infinite dividend or zero divisor return
   the canonical quiet NaN above; NaN payload propagation is not specified.
7. **Coverage check.** `ci/core_library_map.py`, invoked by library_map.sh,
   checks every runtime primitive and arithmetic instruction's direct or
   composite route against both C and Lean codec tables. Aliases cover
   existing spelling differences and List.sum/comparisons are recipes.
   TEXT_QUOTED and CHAR_QUOTED remain explicitly exempt (3.4), outside this
   numeric completion. Core text 0.8 adds `(structural T)` for TEXT_STRUCTURAL;
   it uses the reference structural observer, not user `to_text`.
   Unknown primitives have no exemption.
   Regression mutations remove C-only, Lean-only and both operations.
8. **Proof boundary.** Signatures, range checks and type safety are proved
   for every new operation; numerical functional correctness is tested,
   not proved. Public Core/codec checks cover halfway parity, normal and
   subnormal boundaries, signed zero, overflow, shortest-decimal ties,
   invalid syntax and the reported double-rounding regression. Independent
   differential validation compared 3371 finite patterns' shortest decimal
   values with CPython repr and 3006 decimal texts' bits with Python float.

## D236: files, Float layout and symbolic links (12 Oct 2026)

1. **Construction is the bug boundary.** `Files.in_memory` checks the stored
   map before allocating a state. Keys must have nonempty slash-separated
   components, none `.` or `..`; absolute paths, empty keys and trailing or
   repeated slashes are refused, not normalized. The first malformed key in
   insertion order stops with `Bug.filePathInvalid(path)`, before folders
   are derived or collisions checked. Otherwise a key equal to a folder
   implied by another key stops with `Bug.fileFolderConflict(path)`; `"a"` and `"a/b"` report
   `"a"`, in either order. If several keys conflict, the first conflicting
   file key in insertion order is reported. Valid keys retain their exact
   Text, including Unicode and NUL; dots within a component remain valid.
   Comparisons use
   exact keys and slash-component prefixes, not arbitrary string prefixes
   (`"a"` and `"ab/c"` are distinct). Earlier effects/states remain; the
   refused fake does not allocate an index. Codecs still transport arbitrary
   host worlds without imposing a filesystem consistency invariant.
2. **Kinds are checked at use.** Real modeled handles and fakes use the
   same checks. Every error names the caller's original path, not its
   normalized/full path. Lexical refusal wins (`NotAllowed`); otherwise a
   file used as the capability root or an intermediate folder gives
   `Failed(path, "not a folder")` before checking the target kind. Failed
   operations leave state unchanged. The current exhaustive file-operation
   table includes D237's parent checks, folder creation and persistence:

   | Operation | File target | Folder target | Missing target |
   | --- | --- | --- | --- |
   | `read_text` | `Ok(text)` | `Failed(path, "is a folder")` | `NotFound(path)` |
   | `read_bytes` | `Ok(UTF-8 bytes)` | `Failed(path, "is a folder")` | `NotFound(path)` |
   | `write_text` | replace, `Ok(Unit)` | `Failed(path, "is a folder")` | create if parent exists, `Ok(Unit)`; otherwise `NotFound(path)` |
   | `append_text` | append, `Ok(Unit)` | `Failed(path, "is a folder")` | create if parent exists, `Ok(Unit)`; otherwise `NotFound(path)` |
   | `remove` | delete, `Ok(Unit)` | `Failed(path, "is a folder")` | `NotFound(path)` |
   | `make_folder` | `Failed(path, "not a folder")` | `Ok(Unit)`, unchanged | create target and missing ancestors, `Ok(Unit)` |
   | `list` | `Failed(path, "not a folder")` | `Ok(sorted names)` | `NotFound(path)` |
   | `entries` | `Failed(path, "not a folder")` | `Ok(sorted entries)` | `NotFound(path)` |
   | `exists?` | `true` | `true` | `false` |
   | `within` | retain narrowed root; subsequent Result calls fail `"not a folder"`, `exists?` is false | retain narrowed root, including empty folders | retain narrowed root; Result calls give `NotFound`, `exists?` is false |
   | `read_only` | preserve root/state, change kind to ReadFiles | same | same |
   | `Files.in_memory` | allocate unless a key is also a folder | record every file ancestor; preserve it after removal | empty map allocates an empty root |

   Errors in the table are wrapped in `Error`. ReadFiles operations have
   identical behavior. Any lexically refused path, including every path on
   an inaccessible handle, gives `NotAllowed` for Result calls and false for
   `exists?`; `within` preserves inaccessibility. Traversing a file likewise
   makes `exists?` false. `within`/`read_only` do not return a Result or
   eagerly test existence. D237 replaces D236's implicit-folder creation:
   writes/appends never create parents and removal never removes folders.
   A missing narrowed root gives `NotFound` for all Result calls, including
   `make_folder`; it cannot be recreated through that handle. Malformed host
   worlds remain transportable; no theorem assumes filesystem consistency.
3. **Shortest digits, fixed thresholds.** Float.to_text keeps the nearest
   shortest round-trip decimal, ties to even. It prints fixed notation for
   decimal exponents -6 through 20 and scientific otherwise, matching
   JavaScript Number.toString's layout but omitting the positive exponent's
   `+`. The threshold uses the shortest decimal's exponent: binary64 `1e-6`
   prints `0.000001` even though its exact rational is slightly smaller.
   Its predecessor prints `9.999999999999997e-7`; `1e21` prints `1e21` and
   its predecessor `999999999999999900000`. A whole float has no `.0`;
   both zeros bypass thresholds and print `0`/`-0`. Scientific spelling is
   lowercase `e`, minus only for a negative exponent, with no padding.
   `nan`, `inf`, `-inf` and Float.to_decimal are unchanged. Structural Core
   reports of Float values still use lossless bits, not this Mo text form.
   Independent native `mocore` validation matched 6869 patterns against
   JavaScript Number.toString with D236's special-name/zero/exponent spelling:
   both signs and neighbors of every finite decimal power, plus 3072 seeded
   random patterns. This is regression evidence, not a shortestness proof.
4. **Links are host behavior, not model state.** Every host file operation
   follows relative symbolic links only while resolution stays beneath the
   capability's own folder. Absolute link targets and escapes give
   `NotAllowed(path)` (or false for `exists?`, an inaccessible handle for
   `within`). The reference's 40-link limit and stable
   `Failed(path, "too many links")` reason are also host behavior.
   `remove` follows intermediate links but removes the final
   link itself, including an absolute, escaping or dangling final link;
   it does not open that link's target. `list`/`entries` may follow a link
   in their folder argument, but entries classify children without following
   links, and `walk` never descends into them. Real-folder VM/host tests,
   including narrowed/read-only capabilities and confinement under races,
   must verify this rule; `mocore` has no links or OS I/O and cannot test it.
   No link constructor, resolution algorithm or theorem is added to Lean.

## D237: persistent folders and explicit creation (13 Oct 2026)

1. **State and transport.** `CapState.folders : List String` follows `random`
   as the ninth field; World inherits it before its `fakes` store. C uses
   `mo_core_string_list folders` in both records. It is an ordered set of
   existing non-root folder paths; `""` is the implicit top folder, never
   inserted by operations. Files remain ordered `(path, text)` pairs. Both
   fields are shared through fake identity, `within` and `read_only`.
   Text 0.7 requires the folder list, even when empty; neither codec invents
   folders from transported files, sorts, normalizes or removes duplicates.
   Reports add real folders after files and compare all nine fake fields.
2. **Construction and order.** `Files.in_memory` discovers every proper
   slash-prefix of each raw key, parent-first, in file insertion order; the
   first discovery wins. D236's collision check runs against those folders
   before allocation, preserving its first-conflicting-key rule. File keys
   still are not normalized; invalid/raw keys remain inaccessible or follow
   the existing use-time lexical rules. `make_folder` normalizes its argument
   first, then appends missing prefixes parent-first; existing folders keep
   their places. Removing files leaves the folder list untouched. There is
   no folder-removal operation. `make_folder(".")` succeeds without mutation.
3. **Operation and failure boundary.** `%io.makeFolder : Files, Text →
   Result(Unit, FileError)` corresponds to `FILES_MAKE_FOLDER`; it is ordinary
   host I/O, not spec-only, and has no ReadFiles form. Lexical refusal precedes
   file-root/intermediate checks, then missing-root checks and target kind.
   A file in the way gives `Failed(originalPath, "not a folder")` without
   partially creating ancestors. Writes/appends require the resolved parent
   folder; failures use the caller's full original path, never the parent.
   All errors leave both fields unchanged. The table above covers every
   operation; read-only variants agree. Existing empty folders are observable
   through `list`, `entries`, `exists?`, and narrowed/read-only handles.
4. **Model choices and limits.** `within` remains lazy: a missing-root handle
   becomes usable if a wider handle later creates its root. `make_folder` on
   the missing-root handle itself gives `NotFound`, since making that root
   would act above its authority. Folder membership is authoritative in host
   states, which must supply ancestors for a realistic filesystem; arbitrary
   inconsistent states still decode losslessly and remain type-safe. `entries`
   classifies a path as Folder exactly when the folder list contains it;
   transport does not repair malformed host state. Recursive creation is
   atomic in this model, unlike OS failures that can leave earlier ancestors.
   Links, permissions, OS errors, descriptors and races remain host-only as
   in D236. No implementation error code or logical axiom is added.

## Compiler metadata: decisions

1. **Side tables, not expression fields.** Program.metadata is ignored by
   the calculus. Function/constant tables align with declaration order; body
   tables use preorder over Expr only, including lambda bodies, in wire
   field/list order. Every expression has one optional span. This keeps
   constructors, evaluator and proofs unchanged and makes the C arena/text
   transport sufficient for compilation.
2. **Bindings, not renamed identifiers.** Function parameters have optional
   original names; local bindings use (node, binder slot). Case slots flatten
   the arms; lambda/join slots follow parameter order. Repeated SSA versions
   may share a source name. Generated slots have none. Ignored `_` lambda parameters
   keep their signature type and position with a generated Core name, but no
   source binding/name; NULL checked lambda slots do not remove arguments.
   The compiler resolves
   lexical uses and computes register ranges itself, retaining provenance
   through lifting and optimization; lowering does not guess code offsets.
3. **Stable image identity.** Full function/spec/closure names are independent
   of Core lookup keys, with spec/closure numbers assigned in source order
   before lowering changes the tree. Constants carry full names and ownership;
   their init names append `.init`. Module lists preserve loader/source order.
   `mo_lower` explicitly receives `mo_sources`, whose full file table it copies
   without renumbering; codegen remaps paths to bytecode string indices.
4. **Spec reporting.** Tests carry the decoded sentence; examples/anchors
   carry the verbatim logical line, without outer indentation/line endings
   but with internal spacing/comments. Promises carry their header and a
   nonempty ordered condition-line table. Declarations have spans for listing.
   Subject indices and anchor expectations are explicit, not recovered by
   parsing display names. The draft bytecode debug contract retains all of
   these for architecture 10's tools, including the one-based false-line
   correspondence. It does not change Core evaluation.
5. **Missing provenance.** Handwritten Lean samples use the explicit
   `noSource` mode, not fabricated Mo positions/exposure. Full metadata may
   still contain absent spans for genuinely source-less generated code;
   synthesized code with a responsible source construct inherits its span.
   Source-less images use `%Core` for data/constants and have no exports or
   spec links; direct runs/tests work, generation/anchor checks needing links
   report unavailable. `Metadata.core` is a synthetic full-metadata fixture,
   not a claim about a real source translation.
6. **Validation boundary.** Codecs check shape, indices, ordered unique
   binder records, exact span/parameter/closure coverage, ownership partitions,
   export membership and kind-appropriate links/text/lines. They do not
   certify source truth, offsets against a file's bytes, canonical name
   spelling or subject signature/body correspondence. Lowering owns those.
   Metadata is not included in `wellTyped?` or any outcome. Only the next
   minor text version is accepted; fixtures are regenerated by Lean.

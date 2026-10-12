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
required constant prefix; the library's
choice types `Option(T)`, `Result(T, E)`, `Ordering` and `FileError` are
built in (`ctorsOf`), with their cases in the order reference 8 and 9.2
declare them: `None` before `Some`, `Ok` before `Error`, and `Less`, `Equal`,
`Greater`. A `case` lists its arms in that order.

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
business and leave no trace in Core. A Core variable is the Mo name as a
string, `?` and all (`failed?`). Functions keep their names; where a module
has several functions of one name (5.1), or a type customizes `to_text`, the
lowering qualifies the name: `LineError.to_text`.

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
| `fn(A) : B` | `fn [A] B` | a closure |
| `Console`, `Files`, ... | `cap c` | a handle, `cap c folder` |

The `Unicode16` module is not in this model of Core yet.

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

### 3.4 Text, Char and Bytes

Escapes and text on several lines are the front end's: Core gets the
resulting text. Interpolation lowers to `%text.concat` of the parts, each
converted to its text form:

| Part's type | Its text form in Core |
| --- | --- |
| `Text` | itself |
| `Char` | `%char.toText(c)` |
| `Int` | `%int.toText(n)` |
| `BigInt` | `%big.toText(n)` |
| `Byte` | `%byte.toText(b)`, its decimal digits |
| `Decimal` | `%dec.toText(d)`, the shortest exact spelling |
| `Float` | `%float.toText(x)`: `nan` for a float that is not a number, `inf` and `-inf` for the infinities (reference 9.2); for any other float, reference 9.2 asks for the shortest spelling, which the model doesn't give yet (item 19 of the section on D227) |
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
`MoCore/Examples/Json.lean` declares `Json` with fields `List(Json)` and
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
(`MoCore/Examples/Json.lean`, `read_input`).

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

A `try` inside an expression keeps the order of evaluation; from `json.mo`:
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
Functions retain their source module as metadata. `expose`, hidden insides
and `use` are checks and name resolution, and leave nothing in Core.

### 10.4 Constants

A constant is `ConstDecl`: `const name : T = e`. A use is `global name`,
not a function call. Lowering resolves global names and orders declarations
by their dependencies, including dependencies through called functions
(reference 10.4; bytecode 3.2 breaks ties by module then source order).
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

`Option`, `Result`, `Ordering` and `FileError` are built into Core's data
table (`ctorsOf`); the capability types are Core types; the section 14
functions are primitives or library functions.

## 11. Capabilities and effects

### 11.1 What a function can touch

A capability is a value of type `cap c`: a handle, `cap c root`, where `root`
is the folder a `Files` or `ReadFiles` handle is narrowed to, or nothing for a
handle that reaches nothing (11.3). Every effect is a primitive on a handle
(`%io.print`, `%io.readText`, `%io.now`, ...), which reads and updates the
one `World`: arguments, environment variables, standard input, standard
output and error, the files, the clock's time and the random seed. A function
without a capability parameter can't reach them.

The world's files are texts, as `Files.in_memory` makes them (reference 14),
so `files.read_bytes(path)`, which is `%io.readBytes(files, path)` on `Files`
and on `ReadFiles`, gives the UTF-8 encoding of the file's text: the bytes a
real file holding that text holds. Its failures are `read_text`'s: `NotFound`
for a file that isn't there, and `NotAllowed` for a path that would leave the
folder (11.3). A real file can hold bytes that aren't UTF-8, which
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

In Core this is a wrapper, which takes a `Console` from the runtime even
when `main` takes none:

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
supplies their text forms as failure details; sample helpers use a fixed
detail where deriving those forms isn't modeled yet.

```mo
count("one two\nthree\n") == Counts(lines: 2, words: 3, bytes: 14)
```

```
example count.1() : Unit =
  if %eq(count("one two\nthree\n"), Counts(2, 3, 14)) then Unit
  else fail "count did not match the expected Counts"
```

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
stop, not a failure. The sample helpers `passes` and `scenario` lower their
Boolean conditions to these Unit assertions in spec mode.

### 12.4 Fakes

Core has one `World`. A test's fakes are that world's starting state and the
handles bound to the test's names: `Console.fake()` and
`Console.fake_input(lines)` set standard input; `Files.in_memory(files)` sets
the files (`.read_only` gives a `ReadFiles` handle); `Clock.fixed(t)` sets the
time; `Random.seeded(n)` the seed; `Args.of(list)` and `Env.of(map)` the
arguments and environment. Every use of a fake sees the same world, as
reference 12.4 requires.

The world's standard input is a list of lines, each without its line ending,
which `%io.readLine` returns in turn. The runtime splits what it reads where
`\n` or `\r\n` ends a line, and turns each sequence of bytes that isn't valid
UTF-8 into one U+FFFD, the replacement character (reference 14);
`Console.fake_input(lines)` gives the lines themselves.

In `Files.in_memory`, a folder exists when a file is inside it, and the
folder the fake stands for, `"."` of its handle, always exists; `list` gives
the names directly inside a folder, each subfolder once, sorted, and listing
a folder that doesn't exist is `Error(NotFound(folder))` (reference 14).

`console.printed` and `console.printed_errors` are the primitives
`%io.printed` and `%io.printedErrors`, which read what the world's standard
output and error hold. The checker allows them only in spec code, including
anchors and anonymous functions inside spec code (bytecode 3.3 and 9).
Whether a handle is real or fake remains a host/front-end contract: the
current World model doesn't distinguish them.

Each fake is a handle on its own state (reference 12.4). When a test makes a
new fake of a kind it already has, such as a second `Args.of(...)` for a
second call of `main`, the test is run in steps that share the world, and
the new fake is set between steps (`scenario` in `MoCore/Testing.lean`;
`todo.mo` and `find.mo` need it). Two fakes of one kind in use at the same
time, such as two consoles that both print before either is read, would need
a world per fake. The model doesn't have the fake constructors yet
(architecture 4.1 leaves them to stream 0), and none of the samples needs
them.

`wc.mo`'s test:

```mo
console = Console.fake()
files = Files.in_memory({"a.txt": "one two\n", "b.txt": "three\n"}).read_only
status = main(Args.of(["a.txt", "gone.txt", "b.txt"]), console, files)
assert status == 1
assert console.printed == ["      1       2       8 a.txt", "      1       1       6 b.txt", "      2       3      14 total"]
assert console.printed_errors == ["wc: gone.txt: no such file"]
```

runs, in the world `{ args := ["a.txt", "gone.txt", "b.txt"], files := [("a.txt",
"one two\n"), ("b.txt", "three\n")] }` with `args`, `console` and `files`
bound to handles,

```
let status = main(args, console, files) in
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

| Mo | In Core |
| --- | --- |
| `Int`: `+ - * / %`, unary `-`, `abs`, `min`, `max`, `pow`, `floor_div`, `floor_mod`, `checked_*`, `wrapping_*`, `saturating_*`, `to_decimal`, `to_float`, `to_big`, `to_char`, `to_byte`, `to_text`, `seconds` ... `days` | prim (`%int.*`) |
| `Byte`: `to_int`, `to_text` | prim (`%byte.*`) |
| `BigInt`: `+ - * / %`, unary `-`, `abs`, `to_int`, `to_text` | prim (`%big.*`) |
| `BigInt`: `min`, `max`, `pow`, `floor_div`, `floor_mod` | not modeled |
| `Decimal`: `+ - * / %`, unary `-`, `abs`, `min`, `max`, `round`, `floor`, `ceil`, `to_int`, `to_float`, `to_fixed`, `to_text` | prim (`%dec.*`) |
| `Float`: `+ - * /`, unary `-`, `abs`, `floor`, `ceil`, `round`, `sqrt`, `nan?`, `infinite?`, `to_text` | prim (`%float.*`) |
| `Float`: `to_decimal` | not modeled |
| `<`, `<=`, `>`, `>=` at a built-in type | prim (`%cmp.*`) |
| `==`, `!=` | prim (`%eq`), with `%not` |
| `Text`: `byte_count`, `char_count`, `empty?`, `chars`, `bytes`, `lines`, `words`, `split`, `trim`, `trim_start`, `trim_end`, `starts_with?`, `ends_with?`, `contains?`, `drop_prefix`, `drop_suffix`, `replace`, `repeat`, `pad_left`, `pad_right`, `ascii_lowercase`, `ascii_uppercase`, `to_int`, `to_decimal`, `Text.from_chars`, interpolation | prim (`%text.*`) |
| `Text`: `to_float` | not modeled |
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
| `Args.list`, `Env.get`, `Console.print`, `error`, `read_line`, `printed`, `printed_errors`, `Files.read_text`, `read_bytes`, `write_text`, `append_text`, `remove`, `list`, `exists?`, `within`, `read_only`, `Clock.now`, `Random.int` | prim (`%io.*`); `read_bytes` gives a file's text as UTF-8 (11.1) |
| `FileError`'s text form | Core (`FileError.to_text`) |
| `to_text`, at every type with `Text` | lowered as the part `#{x}` of a text is (3.4), so the two give the same text; a set's is `Lib.setText` (3.4), and the `to_text[T]` the lowering derives for records, choice types, tuples, lists and maps without their own `to_text` is not modeled |
| `Console.fake`, `fake_input`, `Files.in_memory`, `Clock.fixed`, `Random.seeded`, `Args.of`, `Env.of` | the test's world (12.4) |
| `Unicode16` | not modeled |

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
    its own state, and every use of one fake sees the same state. Unchanged:
    the model runs such a test in steps, setting each new fake between them,
    which gives the reference's answer for every sample; two fakes of one kind
    in use at the same time wait for the fake constructors, which
    architecture 4.1 leaves to stream 0 (12.4).
17. **When constants are computed** (10.4): once, before any other code of
    the program runs, in the order of their dependencies; a cycle is an
    error, and a constant that stops stops the program before it starts.
    Now modeled: ordered `ConstDecl`s, prefix checking and once-only
    initialization before entry or tests (10.4).
18. **`min` and `max` with equal elements** (14): the first of them. Already
    did that.
19. **`Float`'s text form** (9.2): numbers have their shortest spelling.
    Unchanged: `%float.toText` prints Lean's `Float.toString`, which is not
    the shortest (`1.5` is `1.500000`). Architecture 4.5 makes it a trusted
    primitive checked against Ryu, and the reference doesn't say how very
    large and very small floats are spelled. "Not a number" and the
    infinities have their spellings since D228 (item 3 of the next section).
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
   return Int; parameter names have no semantic role, and Core has no link
   from a promise to its subject function. Lowering supplies matching input
   and result types, the generated line-number code, and anchor expectations.
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
   `scenario` does this. No image cache is hidden in World. Fuel is a depth
   bound reused for each initializer and the subsequent call, not a shared
   instruction budget. Larger fuel preserves every finished outcome, proven
   for initialization and both runners as well as expressions.
7. **Details in handwritten checks.** `passes` and `scenario` wrap their
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

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
| `42`, `12.50`, `"hi"`, `'c'`, `true`, `Unit` | `.lit` | `int`, `dec`, `str`, `chr`, `bool`, `unitE` |
| `x` | `.var "x"` | `v` |
| `let x = e in body` | `.let_` | `lets`, `seq` |
| `fn(x: T, ...) => body` | `.lam` | `lam` |
| `e(e1, ..., en)` (`e` a function value) | `.app` | `app` |
| `f(e1, ..., en)` (`f` a top-level function) | `.call` | `call` |
| `(e1, ..., en)`, `e.0` | `.tuple`, `.proj` | `tup`, `fst`, `snd` |
| `C(e1, ..., en)` | `.con T "C" [...]` | `mk`, `mkC`, `some_`, `ok_`, ... |
| `case e of C1(x, y) -> e1 \| C2 -> e2 end` | `.case_` | `caseOpt`, `caseRes`, `get` |
| `if c then a else b` | `.if_` | `ite`, `and_`, `or_` |
| `%op(e1, ..., en)` | `.prim` | `prim`, `iadd`, `top`, `lop`, `mop`, `io`, ... |
| `stop "reason"` | `.stop` | `stop` |
| `join j(x: T, ...) = body in rest` | `.join` | `forList`, `whileLoop`, `forRange` |
| `jump j(e1, ..., en)` | `.jump` | `nextRound`, `breakLoop`, `ret` |

Primitives are written with their Lean names: `%int.add` is
`Prim.int .add`, `%cmp.decimal.lt` is `Prim.cmp .decimal .lt`, `%text.isEmpty`
is `Prim.text .isEmpty`, `%list.uncons` is `Prim.list T .uncons`, `%map.set` is
`Prim.map K V .set`, `%eq` is `Prim.eq T`, `%io.print` is `Prim.io .print`.
Their types are in `Prim.sig`.

Core is explicit where Mo infers: function, anonymous-function and join-point
parameters carry their types, and a constructor application names its type.
Names that start with `%` are made by the lowering; Mo names never do. A
program is a data table and a function table (`Program`); the library's
choice types `Option(T)`, `Result(T, E)`, `Ordering` and `FileError` are
built in (`ctorsOf`), with `Some` before `None` and `Ok` before `Error`.

## 1. A first program

The `module` line and `##` comments are not in Core; `main` is an ordinary
function (section 11.2). The `for` loop becomes two join points (section 7.5):

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
      Some(w.p) -> let word = w.p.0 in
                   let w.rest = w.p.1 in
                   let %_ = %io.print(console, %text.upper(word)) in
                   jump w.loop(w.rest)
    | None -> jump w.done()
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
| `Decimal` | `decimal` | `decimal n`, meaning `n × 10⁻¹⁸`, within ±(2¹²⁷ − 1) |
| `Float` | `float` | `float bits` (IEEE 754) |
| `Text`, `Char` | `text`, `char` | `text s`, `char c` |
| `Unit` | `unit` | `unit` |
| `(A, B)` | `tuple [A, B]` | `tuple [a, b]` |
| `List(T)` | `list T` | `list [...]` |
| `Map(K, V)` | `map K V` | `map [(k, v), ...]`, in insertion order |
| `Set(T)` | `map T Unit` | a map to `Unit` |
| `Time`, `Duration` | `time`, `duration` | microseconds (since 1970-01-01T00:00:00Z for a time) |
| `Range` | `tuple [Int, Int]` | `(a, b)` for `a..b` |
| a `struct` or `enum` | `data "Name"` | `con "Case" [fields]` |
| `Option(T)`, `Result(T, E)` | `option T`, `result T E` | `con "Some" [v]`, `con "Error" [e]`, ... |
| `fn(A) : B` | `fn [A] B` | a closure |
| `Console`, `Files`, ... | `cap c` | a handle, `cap c folder` |

`Byte`, `Bytes` and the `Unicode16` module are not in this model of Core yet.

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
| `Decimal` | `%dec.toText(d)`, the shortest exact spelling |
| `Bool` | `if b then "true" else "false"` |
| `Time`, `Duration` | `%time.toText(t)`, `%time.durToText(d)` |
| a type with its own `to_text` | a call of it, such as `LineError.to_text(problem)` |
| `FileError` | `FileError.to_text(e)` (`MoCore/Lib.lean`) |
| any other record, choice type, tuple, list or map | a call of `to_text[T]`, a Core function the lowering derives per type, which quotes texts and characters inside |

```mo
greeting = "Hello, #{name or "you"}! You have #{count} new messages."
```

```
let greeting =
  %text.concat("Hello, ",
    %text.concat(case name of Some(%or) -> %or | None -> "you" end,
      %text.concat("! You have ",
        %text.concat(%int.toText(count), " new messages.")))) in ...
```

The text functions of section 14 are primitives (`%text.lines`,
`%text.split`, ...); see section 14 below.

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
keeps the others' order. A map literal sets its keys in turn, starting from
`%map.empty()`. Equality (`%eq`) compares entries in any order.

```mo
stock = {"apples": 3}
more = stock.set("pears", 5)
count = more.get("apples") or 0
for (fruit, n) in more
  console.print("#{fruit}: #{n}")
end
```

```
let stock = %map.set(%map.empty(), "apples", 3) in
let more = %map.set(stock, "pears", 5) in
let count = case %map.get(more, "apples") of Some(%or) -> %or | None -> 0 end in
join m.done() = ... in
join m.loop(m.rest: List((Text, Int))) =
  case %list.uncons(m.rest) of
    Some(m.p) -> let %e = m.p.0 in let m.rest = m.p.1 in
                 let fruit = %e.0 in let n = %e.1 in
                 let %_ = %io.print(console, %text.concat(fruit, %text.concat(": ", %int.toText(n)))) in
                 jump m.loop(m.rest)
  | None -> jump m.done()
  end
in jump m.loop(%map.entries(more))
```

`Set(T)` is `Map(T, Unit)`: `Set.empty` is `%map.empty()`, `s.add(x)` is
`%map.set(s, x, Unit)`, `s.has?(x)` is `%map.has(s, x)`, `s.to_list` is
`%map.keys(s)`, and `items.to_set` sets each item to `Unit`. Inventory's
`first_by` uses a set this way.

### 3.9 Time and Duration

Both count whole microseconds. `5.seconds` and the other conversions check
for overflow; adding a duration to a time stops outside the years 1 to 9999,
and `checked_add` returns an `Option` instead.

```mo
expires = now + 30.minutes
late? = now - order.placed > 2.days
```

```
let expires = %time.add(now, %int.minutes(30)) in
let late? = %cmp.duration.gt(%time.sub(now, <order.placed>), %int.days(2)) in ...
```

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

Fields named out of declaration order are first bound in source order, so the
evaluation order stays the source's (section 13.1):
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

A function is a `FunDecl`: its name, typed parameters, result type and body.
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
  Some(%m0) -> let _ = %m0.0 in
    case %list.uncons(%m0.1) of
      Some(%m1) -> let category = %m1.0 in
        case %list.uncons(%m1.1) of
          Some(%m2) -> let amount = %m2.0 in
            case %list.uncons(%m2.1) of
              Some(%_) -> jump %other()
            | None -> <arm>
            end
        | None -> jump %other()
        end
    | None -> jump %other()
    end
| None -> jump %other()
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
`case %io.readLine(console) of Some(line) -> ... | None -> <leave the loop>`
(`MoCore/Examples/Json.lean`, `read_input`).

### 7.5 Loops

A loop is two join points. `l.loop` takes the loop's position and the `var`s
the body assigns, and runs one round; `l.done` takes the `var`s and runs what
follows the loop. Every path through the body ends by jumping: to `l.loop`
for the next round (the end of the body, or `continue`), or to `l.done`
(`break`). A `for` over a list takes it apart with `%list.uncons`; over a map,
its `%map.entries`; over a set, its keys; over a range `a..b`, an `Int` that
counts up to `b`.

```mo
var total = 0
for i in 0..10
  continue if i % 2 == 0
  total += i
end
```

```
let total = 0 in
let r.end = 10 in
join r.done(total: Int) = <what follows> in
join r.loop(i: Int, total: Int) =
  if %cmp.int.lt(i, r.end) then
    if %eq(%int.rem(i, 2), 0) then jump r.loop(%int.add(i, 1), total)
    else jump r.loop(%int.add(i, 1), %int.add(total, i))
  else jump r.done(total)
in jump r.loop(0, total)
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

A range kept in a variable is the pair of its ends: `r = 0..10` is
`let r = (0, 10) in`, and `for i in r` counts from `r.0` up to `r.1`;
`r.to_list` is `%int.range(r.0, r.1)` and `r.contains?(n)` is
`r.0 <= n and n < r.1`.

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

`x or default` is a `case` that evaluates `default` only for `None` (like the
Bool `or`, reference 13.1). `x.or_fail(e)` is a library call, so `x` and `e`
are both evaluated first:

```mo
city = address.city or "unknown"
found = users.get(id).or_fail(NoUser(id: id))
```

```
let city = case <address.city> of Some(%or) -> %or | None -> "unknown" end in
let found = let %opt = %map.get(users, id) in
            let %fail = NoUser(id) in
            case %opt of Some(%x) -> Ok(%x) | None -> Error(%fail) end in ...
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
    Some(n) -> if %cmp.int.lt(n, 0) then Error(Negative(n)) else Ok(n)
  | None -> Error(NotANumber(trimmed))
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
      case parse_quantity(case %list.get(parts, 1) of Some(%or) -> %or | None -> "" end) of
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
  Some(%digit) -> jump d.loop(d.rest, %int.add(%left, %digit))
| None -> None
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
             Some(r) -> r
           | None -> stop "every currency was checked against the rate table at startup"
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
| `Equal` (`==`, `!=`) | `%eq` at the type: structural equality, maps and sets by their entries in any order, floats by IEEE. `a != b` is `%not(%eq(a, b))`. |
| `Hash` | nothing to call: map and set primitives compare keys with the same structural equality. |
| `Order` | for `Int`, `BigInt`, `Decimal`, `Float`, `Bool`, `Char`, `Text`, `Time` and `Duration`, the primitives `%cmp.b.lt`, `le`, `gt`, `ge`; for a record or choice type, a Core function `compare[T]`, the module's own `compare` or one the lowering derives (fields in order, then cases in order); `sort`, `min`, `max`, `sort_by` are Core functions instantiated with it. |
| `Text` | the type's text form, as in 3.4. |
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

Modules exist only for the front end: Core is one whole program, with every
name resolved to a declaration. `expose`, hidden insides and `use` are
checks and name resolution, and leave nothing in Core.

### 10.4 Constants

A constant is a function with no parameters, and a use of it is a call. A
constant's value is fixed, so calling it at each use gives the same value. To
compute constants "once, before `main` starts" and stop before `main` when one
stops, the runtime's entry point evaluates each constant once before calling
`main` (11.2).

```mo
max_attempts = 3
t0 = Time.parse("2026-10-12T09:00:00Z") or stop "the time is written correctly"
```

```
fn max_attempts() : Int = 3
fn t0() : Time = case %time.parse("2026-10-12T09:00:00Z") of
                   Some(%or) -> %or
                 | None -> stop "the time is written correctly"
                 end
```

### 10.5 Always in scope

`Option`, `Result`, `Ordering` and `FileError` are built into Core's data
table (`ctorsOf`); the capability types are Core types; the section 14
functions are primitives or library functions.

## 11. Capabilities and effects

### 11.1 What a function can touch

A capability is a value of type `cap c`: a handle, `cap c folder`, where
`folder` is what a `Files` or `ReadFiles` handle is narrowed to. Every effect
is a primitive on a handle (`%io.print`, `%io.readText`, `%io.now`, ...),
which reads and updates the one `World`: arguments, environment variables,
standard input, standard output and error, the files, the clock's time and
the random seed. A function without a capability parameter can't reach them.

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
`%io.readOnly(files)`, a `ReadFiles` handle on the same folder. A path is
relative, uses `/`, and can't leave the handle's folder (no `..`).

### 11.4 Rules

That anonymous functions don't capture capabilities, and that fakes appear
only in tests, examples and promises, are the front end's checks. Core's
typing doesn't need them for safety.

## 12. Examples, promises and tests

### 12.1 Examples

Each line of an `examples` block is a Core expression of type `Bool`; `mo
test` checks that it evaluates to `true`. Equality is `%eq` at the compared
type.

```mo
count("one two\nthree\n") == Counts(lines: 2, words: 3, bytes: 14)
```

```
%eq(count("one two\nthree\n"), Counts(2, 3, 14))
```

### 12.2 Promises

A promise's body becomes a Core function of the parameters and `result`,
returning `Bool` (several lines are joined with `and`). Its anchors are calls
of that function: each `allows` must give `true`, each `forbids` `false`. `mo
test` then calls it on generated inputs with the function's actual result.

```mo
promise "a deposit adds exactly the amount"
  result is Ok(after) implies after.balance == account.balance + amount
  allows account: alice(10.00), amount: 5.50, result: Ok(alice(15.50))
  forbids account: alice(10.00), amount: 5.50, result: Ok(alice(10.00))
end
```

```
fn deposit.promise.1(account: Account, amount: Decimal, result: Result(Account, AccountError)) : Bool =
  case result of
    Ok(after) -> %eq(<after.balance>, %dec.add(<account.balance>, amount))
  | Error(%_) -> true
  end

-- allows:  deposit.promise.1(alice(10.00), 5.50, Ok(alice(15.50)))           is true
-- forbids: %not(deposit.promise.1(alice(10.00), 5.50, Ok(alice(10.00))))     is true
```

### 12.3 Tests

A test is a Core expression; `assert e` is
`if e then <rest> else stop "assertion failed: ..."`, and
`assert e is Pattern` is a `case` that holds the rest of the test in the
matching arm and stops in the others. The test passes when it returns.

### 12.4 Fakes

Core has one `World`. A test's fakes are that world's starting state and the
handles bound to the test's names: `Console.fake()` and
`Console.fake_input(lines)` set standard input; `Files.in_memory(files)` sets
the files (`.read_only` gives a `ReadFiles` handle); `Clock.fixed(t)` sets the
time; `Random.seeded(n)` the seed; `Args.of(list)` and `Env.of(map)` the
arguments and environment. `console.printed` and `console.printed_errors` are
the primitives `%io.printed` and `%io.printedErrors`, which read what the
world's standard output and error hold. Every use of a fake sees the same
world, as reference 12.4 requires.

When a test makes a new fake of a kind it already has, such as a second
`Args.of(...)` for a second call of `main`, the test is run in steps that
share the world, and the new fake is set between steps (`scenario` in
`MoCore/Testing.lean`; `todo.mo` and `find.mo` need it).

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
right, then the call. `and`, `or`, `implies` and the Option `or` lower to
`if` and `case`, which evaluate only the branch taken. The lowering keeps Mo's
order wherever it reorders anything (record fields named out of order, 4.1;
`try` inside an expression, 8.3).

### 13.2 Bugs

A bug ends evaluation with `stopped`, its reason, and the world so far:

| Reference 13.2 | `Bug` |
| --- | --- |
| overflow in `Int`, `Decimal` or `Duration` arithmetic; a time outside the years 1 to 9999 | `overflow what` |
| dividing by zero | `divByZero` |
| reaching a `stop` | `stop reason` |
| calling a library function outside what it accepts (`take(-1)`, `random.int(5, 5)`, `split("")`, ...) | `outside call` |

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
| `Int`: `+ - * / %`, unary `-`, `abs`, `min`, `max`, `pow`, `floor_div`, `floor_mod`, `checked_*`, `wrapping_*`, `saturating_*`, `to_decimal`, `to_float`, `to_big`, `to_char`, `to_text`, `seconds` ... `days` | prim (`%int.*`) |
| `Int`: `to_byte` | not modeled |
| `BigInt`: `+ - * / %`, unary `-`, `abs`, `to_int`, `to_text` | prim (`%big.*`) |
| `BigInt`: `min`, `max`, `pow`, `floor_div`, `floor_mod` | not modeled |
| `Decimal`: `+ - * / %`, unary `-`, `abs`, `min`, `max`, `round`, `floor`, `ceil`, `to_int`, `to_float`, `to_fixed`, `to_text` | prim (`%dec.*`) |
| `Float`: `+ - * /`, unary `-`, `abs`, `floor`, `ceil`, `round`, `sqrt`, `nan?`, `infinite?`, `to_text` | prim (`%float.*`) |
| `Float`: `to_decimal` | not modeled |
| `<`, `<=`, `>`, `>=` at a built-in type | prim (`%cmp.*`) |
| `==`, `!=` | prim (`%eq`), with `%not` |
| `Text`: `byte_count`, `char_count`, `empty?`, `chars`, `lines`, `words`, `split`, `trim`, `trim_start`, `trim_end`, `starts_with?`, `ends_with?`, `contains?`, `drop_prefix`, `drop_suffix`, `replace`, `repeat`, `pad_left`, `pad_right`, `ascii_lowercase`, `ascii_uppercase`, `to_int`, `to_decimal`, `Text.from_chars`, interpolation | prim (`%text.*`) |
| `Text`: `bytes`, `to_float` | not modeled |
| `Char`: `code`, `ascii_digit?`, `ascii_letter?`, `ascii_white_space?`, `ascii_lowercase`, `ascii_uppercase`, `to_text` | prim (`%char.*`) |
| `List`: literals, `size`, `empty?`, `get`, `first`, `last`, `append`, `prepend`, `concat`, `reverse`, `take`, `drop`, `set`, `remove_at`, `enumerate`, `zip`, `contains?`, `unique` | prim (`%list.*`) |
| `List`: `join` | prim (`%text.join`) |
| `List`: `to_map` | prim (`%map.ofList`) |
| `List`: `map`, `filter`, `filter_map`, `find`, `any?`, `all?`, `count`, `reduce`, `max`, `sum`, `sort_by`, `sort_by_desc` | Core (`List.map[A,B]`, ...) |
| `List`: `sort`, `min` | lowered: `sort` is `sort_by` with the element itself as key, `min` is `max` with the order reversed (not written out in `Lib.lean`) |
| `List`: `flat_map`, `group_by`, `to_set` | lowered the same way as `map` (a loop with join points), not written out in `Lib.lean` |
| `Map`: literals, `size`, `empty?`, `get`, `has?`, `set`, `remove`, `keys`, `values`, `entries` | prim (`%map.*`) |
| `Map`: `update` | Core (`Map.update[K,V]`) |
| `Set`: `Set.empty`, `size`, `empty?`, `has?`, `add`, `remove`, `to_list` | prim, on `Map(T, Unit)` |
| `Set`: `union`, `intersection`, `difference` | lowered as loops over keys, not written out |
| `Range`: `for`, `to_list`, `contains?` | a counting loop; `%int.range`; `a <= n and n < b` |
| `Option`: `or`, `or_fail`, `map`; `Result`: `map`, `map_error` | lowered: a `case` |
| `Time`: `+ -` with durations, `-` of two times, `checked_add`, `to_text`, `Time.parse`; `Duration`: `+ - *`, `in_milliseconds`, `in_seconds`, `to_text` | prim (`%time.*`) |
| `Args.list`, `Env.get`, `Console.print`, `error`, `read_line`, `printed`, `printed_errors`, `Files.read_text`, `write_text`, `append_text`, `remove`, `list`, `exists?`, `within`, `read_only`, `Clock.now`, `Random.int` | prim (`%io.*`) |
| `Files.read_bytes` | not modeled |
| `FileError`'s text form | Core (`FileError.to_text`) |
| `Console.fake`, `fake_input`, `Files.in_memory`, `Clock.fixed`, `Random.seeded`, `Args.of`, `Env.of` | the test's world (12.4) |
| `Unicode16` | not modeled |

## Ambiguities found while translating

These places in the reference left a choice open; the model's choice is
given, and each is worth settling in the reference.

1. **Map literals with a repeated key** (3.8). "A literal that gives the same
   key twice is an error", but keys are expressions (`{fruit: 3}`), so two
   keys can turn out equal only at run time. The model sets keys in order:
   the later value wins and the key keeps its first place. `List.to_map` with
   a repeated key is the same question (14).
2. **`x or default` and `or_fail`** (8.1, 13.1). 13.1 says `and`, `or` and
   `implies` skip their right side; it doesn't say whether this includes the
   `Option` `or`. The samples need it to (`t0 = Time.parse(...) or stop
   "..."` would always stop otherwise), so the model's `or` is lazy.
   `or_fail(e)` is a function, so `e` is evaluated even for `Some`.
3. **Evaluation order of named fields** (4.1, 13.1). Fields may be named in
   any order, and evaluation is "left to right", but left to right in the
   source or in the declaration? It matters when two fields have effects or
   can stop. The model keeps source order.
4. **`try` and `return` inside an anonymous function** (5.6, 8.3). "Returns
   the Error from the function it is in": the anonymous function, or the
   named function around it? The model takes the innermost function.
5. **`lines` and line endings** (3.4, 14). "Without their line endings":
   is `\r\n` a line ending? The model drops a `\r` before each `\n`.
6. **ASCII white space** (14, for `words`, `trim` and `ascii_white_space?`).
   Which characters? The model uses space, tab, line feed, vertical tab, form
   feed and carriage return.
7. **`"".split(",")`** (14). `[""]` or `[]`? Empty pieces are kept, so the
   model gives `[""]`.
8. **`Text.to_decimal`** (14). Is `"1."` or `".5"` accepted ("an optional `.`
   followed by up to 18 digits": may there be zero digits)? The model
   requires a digit before and after the point. `to_int` accepts leading
   zeros and `-0`, as the letter of 14 allows.
9. **Decimal `%`** (3.2, 9.2). `Number` gives `%` to `Decimal`, but only the
   whole-number `%` is described. The model's remainder has the sign of the
   left side, like `Int`'s.
10. **`round(places)` and `to_fixed(places)` outside 0 to 18** (3.3, 14).
    Negative places (rounding to tens) or more than 18 places: allowed? The
    model treats them as calls outside what the function accepts (a bug).
11. **Other calls "outside what it accepts"** (13.2, 14). Section 14 says
    which arguments `take`, `drop`, `pow`, `split` and `Random.int` accept;
    for others it doesn't. The model stops on `repeat(-1)` and
    `replace("", new)` (an empty text occurs everywhere), and leaves text
    unchanged for `pad_left` and `pad_right` with a negative width.
12. **`FileError`'s text form** (14). Only `NotFound` is given (`notes/a.md:
    no such file`). The model writes `path: not allowed`, `path: not text`
    and `path: reason` for the others.
13. **Folders in `Files.in_memory`** (12.4, 14). A map of paths to texts has
    no folders of its own. The model says a folder exists when a file is
    inside it (the capability's own folder always exists); `list` gives the
    names directly inside, subfolders once each, sorted; listing a missing
    folder is `Error(NotFound(folder))`.
14. **`within` a folder that leaves the capability's** (11.3, 14). `within`
    returns a `Files`, not a `Result`, so `files.within("..")` can't fail
    politely. The model makes it a bug.
15. **A time's text form and `Time.parse`** (3.9, 14). RFC 3339 allows
    `Z` or `+00:00`, any number of fraction digits, lowercase `t` and `z`,
    and leap seconds. The model prints `Z` and fractions only when there are
    some; it parses `T` or `t`, `Z`, `z` or an offset, and one to six fraction
    digits, and rejects second 60.
16. **Several fakes of one kind in a test** (12.4). `find.mo` makes two
    `Console.fake()`s and `todo.mo` four `Args.of(...)`s. Core's one world
    handles this by running the test in steps; two fakes of one kind used
    at the same time would need a world per fake.
17. **When constants are computed** (10.4). "Once, before `main` starts":
    tests and examples have no `main`. The model computes a constant where
    it is used (it is pure, so the value is the same); the runtime's entry
    point computes them all first, so a stopping constant stops before
    `main`.
18. **`List.max` with equal largest elements** (14). Which one? It matters
    only for a customized `Order` whose equal values differ. The model
    returns the first.
19. **`Float`'s text form** (9.2). "Numbers have their shortest spelling";
    the model prints Lean's `Float.toString` instead, which is not that.
20. **A `var` read by an anonymous function and assigned afterwards** (5.6,
    6.2). An anonymous function "can read names in scope where it is made";
    does it see the value then, or the `var`'s later value? The model's
    closures capture the value when the function is made.
21. **`exists?` for a path that would leave the folder** (11.3, 14). The
    model answers `false` (reading such a path is `Error(NotAllowed(path))`).
22. **`printed` on a real console** (12.4, 14). The reference gives
    `printed` and `printed_errors` to fakes, but a real console has the same
    type `Console`. The model's `%io.printed` reads what the world's standard
    output holds, for any console handle.
23. **`Range` values** (3, 7.5). `Range` is in the table of types, but only
    `for`, `to_list` and `contains?` are said of its values. The model makes a
    range the pair of its ends.

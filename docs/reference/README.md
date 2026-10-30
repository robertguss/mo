# Mo Language Reference, v0.1

**Version 0.1, approved on 12 October 2026.** This is core Mo, the language that later
versions build services, live upgrades and approved intent on. It is written to
be read in full, by people and by agents, before writing Mo.

Every rule has an example, and every code example parses with Mo's executable
grammar (`grammar/` in this repository). "An error" below means the compiler
rejects the program. "Stops" means a running program has hit a bug and ends
with a report (section 13.2).

## 1. A first program

```mo
## Prints each word given on the command line, in capitals.
module Shout

fn main(args: Args, console: Console)
  for word in args.list
    console.print(word.ascii_uppercase)
  end
end
```

- A file is one module, named on its first line of code. `##` comments
  describing it may come before.
- `main` lists the capabilities the program needs: here its arguments and the
  console. They are its whole authority. It can't touch files, the network or
  the clock, because it wasn't handed them (section 11).
- Blocks open with a keyword and close with `end`. Indentation carries no
  meaning; `mo format` lays code out with two spaces per level, in lines of at
  most 100 columns.

## 2. Source text

### 2.1 Files

A program is a tree of modules under one root folder. The module
`Shop.OrderBook` lives in `shop/order-book.mo`: each part of its name becomes a
lowercase folder or file name, with hyphens between words. A word starts at a
capital letter that follows a lowercase letter or a digit, and at the last
capital of a run that a lowercase letter follows, so `XMLReader` lives in
`xml-reader.mo`, `HTTP2Server` in `http2-server.mo` and `Unicode16Tool` in
`unicode16-tool.mo`.

Source is UTF-8. Characters that change how text looks without being seen are
an error anywhere, even in text and comments: direction overrides, zero-width
characters, and control characters other than tab and line breaks. In text,
write them as escapes, such as `"\u{200B}"`.

### 2.2 Comments

`#` starts a comment that runs to the end of the line. `##` documents the
declaration below it, or the module when it comes before `module`. `mo format`
lines up the comments at the ends of consecutive lines in one column, two
spaces after the longest line's code.

<!-- mo-example: fragment -->
```mo
## The price of an order, with tax.
fn total(order: Order) : Decimal
  order.subtotal + order.tax  # the tax is already rounded
end
```

### 2.3 Names

- Values, functions, fields and parameters are snake_case: lowercase ASCII
  letters, digits and `_`, starting with a letter, or with `_` for a name that
  is unused on purpose.
- Types, the cases of choice types, and modules are CapCase: `OrderBook`,
  `NotFound`.
- A type parameter is a single capital letter: `T`, `K`, `V`.
- A name ending in `?` is a yes-or-no answer: a `Bool`, or a function that
  returns one, such as `empty?` or `paid?`.
- `_` alone means "ignored", and a name starting with `_`, such as `_reason`,
  is unused on purpose.

Names are ASCII; text and comments may use any Unicode. These words are
reserved:

```text
allows and assert break case continue else end enum examples expose false fn
for forbids if implies in is module not or promise return service stop struct test true
try use var where while
```

`service` is reserved. The service-clause words `state`, `tools`, `every`,
`token`, `from` and `env` remain ordinary identifiers outside their clause
positions (section 11.5).

### 2.4 Lines

A line break ends a statement; there are no semicolons. A statement continues
on the next line when its line ends with an opening bracket, a comma, `.`, `=`
or a binary operator, or when the next line starts with a closing bracket, `.`,
`where` or a binary operator other than `-`. A statement also runs on through
any block it opens, such as a `case` or an anonymous function written on
several lines, to that block's `end`.

<!-- mo-example: fragment -->
```mo
order = Order(
  id: 7,
  lines: [],
  paid?: false
)
names = customers
  .filter(fn(c) c.active? end)
  .map(fn(c) c.name end)
ready = stocked and
  priced
```

These show where a statement may break. `mo format` chooses the breaks itself:
a statement that fits in 100 columns goes on one line, so it would join each of
these, and a longer one is broken at its outermost brackets, dots or operators
first. It counts columns as `Unicode16.display_width` does (section 14), turns
any run of blank lines between statements into one, and writes no trailing
commas.

## 3. Values and types

| Type | Values | Section |
| --- | --- | --- |
| `Bool` | `true` and `false` | 3.1 |
| `Int`, `BigInt`, `Byte` | whole numbers | 3.2 |
| `Decimal`, `Float` | fractions | 3.3 |
| `Text`, `Char`, `Bytes` | Unicode text, one character, binary data | 3.4 |
| `Unit` | just `Unit`, what a function that returns nothing returns | 3.5 |
| `(A, B)` | tuples | 3.6 |
| `List(T)`, `Map(K, V)`, `Set(T)` | collections | 3.7, 3.8 |
| `Range` | whole numbers from `a` up to `b`, written `a..b` | 7.5 |
| `Time`, `Duration` | instants and lengths of time | 3.9 |
| records and choice types | declared with `struct` and `enum` | 4 |
| `fn(A) : B` | functions | 5.6 |
| `Option(T)`, `Result(T, E)` | maybe-missing values, and success or failure | 8 |
| capabilities | `Console`, `Files` and the others | 11 |

All values are immutable. A `var` (section 6.2) changes which value a name
holds, never a value someone else holds. Mo never converts between types
silently: a conversion is a function call, and one that could fail returns an
`Option`. Conversions that round, such as `to_float` and `to_fixed`, say how
they round. Comparing or combining values of different types is an error.

### 3.1 Bool

`and` and `or` evaluate their right side only when they need it, and `not`
negates. A condition must be a `Bool`: no numbers, texts or lists stand in for
one.

<!-- mo-example: fragment -->
```mo
ready = in_stock and not on_hold
if order.lines.empty?
  console.print("nothing to ship")
end
```

`a implies b` means `not a or b`. It reads best in promises (section 12.2).

### 3.2 Whole numbers

`Int` is a 64-bit signed whole number, and the type of counts, sizes and
positions. Literals may put `_` between digits, and may be hexadecimal or
binary:

```mo
population = 8_100_000_000
mask = 0xFF
flags = 0b1010
```

`+`, `-`, `*`, `/` and `%` take two numbers of the same type. `/` rounds toward
zero, and `%` gives the remainder with the sign of the left side: `-7 / 2` is
`-3`, and `-7 % 2` is `-1`. `a.floor_div(b)` and `a.floor_mod(b)` round toward
negative infinity instead.

Overflow, and dividing by zero, are bugs: the program stops. Code that expects
a result might not fit says what it wants instead:

<!-- mo-example: fragment -->
```mo
fits = a.checked_add(b)            # an Option(Int): None if it doesn't fit
hash = h.wrapping_mul(31)          # wraps around
capped = level.saturating_add(10)  # caps at the largest Int
```

`BigInt` holds whole numbers of any size, with the same arithmetic: `n.to_big`
converts to one, and `big.to_int` returns `None` when the value doesn't fit. `Byte` holds 0 to 255,
for binary data; `n.to_byte` returns an `Option(Byte)`. `Byte` has no
arithmetic (section 9.2): convert it with `to_int` first.

### 3.3 Decimal and Float

A literal with a decimal point, such as `19.99`, is a `Decimal`: an exact
fraction with 18 decimal places, up to about ±1.7 × 10²⁰. Decimal arithmetic is
exact while results fit in 18 places, so `0.1 + 0.2 == 0.3`. A result that
needs more places, such as `1.00 / 3`, is rounded to the nearest 18th place,
with an exact tie rounded to the even digit. Overflow and dividing by zero stop
the program, as with `Int`, and `%` gives the remainder with the sign of the
left side, as it does for `Int`. A decimal's text form is its shortest exact
spelling: `15.5`, `3`, `0.001`.

`Float` is a 64-bit IEEE 754 binary floating-point number, for science and
graphics. A number literal takes the type its context needs: in `x * 0.5`, with
`x` a `Float`, `0.5` is a `Float`, and in `price * 2`, with `price` a
`Decimal`, `2` is a `Decimal`. A literal with nothing to decide its type is an
`Int`, or a `Decimal` if it has a decimal point. Dividing an `Int` by an `Int`
gives an `Int`, so convert first to get a fraction:
`total.to_decimal / count.to_decimal`. Converting is explicit:

<!-- mo-example: fragment -->
```mo
share = cents.to_decimal / 100  # Int to Decimal
ratio = share.to_float          # Decimal to Float, rounded to the nearest float
back = ratio.to_decimal         # an Option: a float may be infinite or not a number
rounded = share.round(2)        # 2 places, ties away from zero, as people round by hand
label = rounded.to_fixed(2)     # the text "0.10": exactly 2 places, rounded as round does
```

### 3.4 Text, Char and Bytes

`Text` is Unicode text. It is stored as UTF-8 and is always valid. Text is
written in double quotes, with the escapes `\n`, `\t`, `\r`, `\\`, `\"`, `\'`,
`\#` and `\u{1F600}` (a character by its hexadecimal code). `#{...}` inside text
inserts the text form of any value that has one (section 9.2), and may hold any
expression, even one with text in it, as in `"#{name or "you"}"`. Lines inside
`#{...}` break as they do elsewhere (section 2.4), with `#{` and its `}` as
brackets, so a `case` may run over several lines there:

<!-- mo-example: fragment -->
```mo
greeting = "Hello, #{name}! You have #{count} new messages."
literal = "write \#{name} to insert a name"
label = "#{count} #{case count
  1: "item"
  _: "items"
end}"
```

Text on several lines starts with `"""` at the end of a line and ends with
`"""` first on a line, after its indentation; code may go on after it on that
line, as in `""")`. It holds the lines between them, without the line break
before the closing `"""`, which sets the indentation every line drops:

```mo
usage = """
  Usage: report [--json] FILE
  Prints a summary of FILE.
  """
```

A `Char` is one Unicode character (a code point), in single quotes: `'a'`,
`'\n'`, `'é'`.

Text has no single "length". `text.byte_count` counts UTF-8 bytes, instantly;
`text.char_count` counts characters, reading the text. There is no `text[i]`:
use whole-text operations such as `split`, `lines`, `trim`, `starts_with?` and
`replace`, or walk `text.chars`. Two texts are equal when their bytes are
equal, and texts sort by character code. Mo never normalizes text silently.
The `Unicode16` module (section 14) normalizes text, splits it into
user-perceived characters, and converts case beyond ASCII.

`Bytes` is binary data. `text.bytes` always succeeds; `bytes.decode_utf8`
returns `None` when the bytes aren't valid UTF-8.

### 3.5 Unit

`Unit` is the type with one value, also written `Unit`. A function with no
result type returns it. `Result(Unit, E)` is the result of an action that can
fail but has nothing to return.

### 3.6 Tuples

A tuple groups two or more values: `(3, "three")` has type `(Int, Text)`.
`pair.0` and `pair.1` read its parts, and a pattern takes it apart:

<!-- mo-example: fragment -->
```mo
(low, high) = bounds(readings)
spread = high - low
```

### 3.7 Lists

`List(T)` is Mo's sequence: `[1, 2, 3]`, or `[]` when empty. Every element has
the same type. Reading by position returns an `Option`, because the position
may be past the end:

<!-- mo-example: fragment -->
```mo
first = items.first       # an Option
third = items.get(2)      # positions start at 0
longer = items.append(4)  # a new list; items itself is unchanged
```

Functions that "change" a list return a new one, so `items.append(4)` on a line
of its own is an error: the new list is unused. With a `var`, write
`items = items.append(4)`. When the old list has no other holder, the new one
reuses its memory, so building a list in a loop costs what it would with a
mutable array (section 13.3).

### 3.8 Maps and sets

`Map(K, V)` maps keys to values: `{"apples": 3, "pears": 5}`, or `{}` when
empty. Keys are expressions, so `{fruit: 3}` uses the value of `fruit`. A
literal that gives the same key twice is an error when the compiler can see
it, as with two equal literals or the same name twice, and a bug when two keys
turn out equal as the program runs. `Set(T)` holds
distinct values: `Set.empty`, a constant, or `items.to_set`. Keys and set members need
equality and hashing, which most types have (section 9.2).

Maps and sets keep insertion order, also after removals: iterating visits keys
in the order they were added, setting an existing key keeps its place, and a
key removed and added again goes last. Two maps are equal when they hold the same entries, in any order.
Iterating a map gives `(key, value)` tuples.

<!-- mo-example: fragment -->
```mo
stock = {"apples": 3}
more = stock.set("pears", 5)
count = more.get("apples") or 0  # get returns an Option
for (fruit, n) in more
  console.print("#{fruit}: #{n}")
end
```

### 3.9 Time and Duration

A `Time` is an instant between the years 1 and 9999, read from a `Clock`
(section 11). A `Duration` is a length of time, made from an `Int`:
`5.seconds`, `200.milliseconds`, `3.minutes`, `2.hours`, `7.days`. Each unit also
accepts its singular spelling: `second`, `millisecond`, `minute`, `hour`, `day`.
Either spelling works with any `Int`, including qualified calls and function
values such as `Int.minute`. Both times and durations count
whole microseconds. Subtracting two times gives a `Duration`, and never
overflows; adding a `Duration` to a `Time`, or subtracting one from it, gives
a `Time`; durations add, subtract and multiply by an `Int`. A time outside the
years 1 to 9999 is a bug, so `time.checked_add(duration)` returns an `Option`,
`None` outside them, for code that can't rule it out; a negative duration goes
back in time. Times and durations compare with `<` and the other comparisons.

`mo format` writes a singular unit on a direct integer-literal receiver whose
value is 1, including hexadecimal and binary literals, and a plural unit on
other integer literals. It leaves other receivers and qualified function names
as written: `n.minute`, `record.seconds`, `(1).minutes`, `(-1).second`, and
`Int.minute`. In `-1.seconds`, the receiver is the literal 1, so it writes
`-1.second`. The formatter uses syntax, not types; this boundary avoids renaming
unrelated fields or functions (D303,
[wording approval](https://ampcode.com/threads/T-01a1705d-1274-74f9-8fcf-0bf58e6f89b8)).

<!-- mo-example: fragment -->
```mo
expires = now + 30.minutes
an_hour_ago = now - 1.hour
late? = now - order.placed > 2.days
```

## 4. Records and choice types

### 4.1 Records

A `struct` declares a record type: named fields, each with a type. A record is
built by naming every field, in any order, and its fields are read with `.`:

```mo
struct Point
  x: Decimal
  y: Decimal
end

origin = Point(x: 0, y: 0)
sum = origin.x + origin.y
```

When a record is built, its fields are computed in the order they are written
there, whatever their order in the declaration. There are no optional fields; a
field that may be missing has an `Option` type. Records have no methods:
functions in the record's module work on it (section 5.4).

### 4.2 Choice types

An `enum` declares a choice type: each value is exactly one of its cases. A
case carries no data, one unnamed value, or named fields:

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

A case is built like a record, by naming its fields, or from its one unnamed
value: `Circle(2.5)`. Types may be generic and recursive:

```mo
enum Tree(T)
  Leaf
  Node(left: Tree(T), value: T, right: Tree(T))
end
```

A module's cases share one name space: no two of its enums have a case of the
same name, and no case reuses the name of a type or case that is always in
scope, such as `Set`, `None` or `Error`. `Option` and `Result` are ordinary choice types (section 8).

## 5. Functions

### 5.1 Definitions

```mo
fn shipping(weight: Decimal, express?: Bool) : Decimal
  base = 4.50 + weight * 0.75
  if express?: base * 2 else: base
end
```

A signature gives every parameter's type and the result's type; a function
with no result type returns `Unit`. Inside a body, types are inferred from all
their uses in it. A body is a sequence of statements, and its value is the
value of its last line. `return`
leaves early. A trailing `if`, a guard, is allowed only on the three early
exits: `return`, `break` and `continue`.

```mo
fn discount(total: Decimal, member?: Bool) : Decimal
  return 0 if not member?
  return total * 0.10 if total > 100
  total * 0.05
end
```

Functions are declared at the top level of a module. There are no default or
optional parameters. Two functions in a module share a name only when their
first parameters are different types declared in that module, so both `Order`
and `OrderLine` can have a `total`; a call picks the one whose first parameter
matches.

### 5.2 Calls

Arguments are passed by position: `shipping(2.5, false)`. Only records and
cases are built with named fields. A function with no parameters is called
with empty parentheses: `defaults()`.

### 5.3 Recursion

Functions may call themselves and each other. A call in tail position, the last
thing a function does, takes the caller's place on the stack instead of adding
to it, so a recursive loop can run as long as it needs to:

```mo
fn sum_to(n: Int, total: Int) : Int
  if n == 0: total else: sum_to(n - 1, total + n)
end
```

### 5.4 Dot calls

`x.f(a, b)` calls `f(x, a, b)`, and `x.f`, with no other arguments, calls
`f(x)`: write `x.f`, not `x.f()`. Mo looks for `f` first in the module that declares the type of `x`, then
among the names in scope. Chains read left to right: `text.trim.lines.size`.

<!-- mo-example: fragment -->
```mo
words = sentence.split(" ")  # Text's split
loud = shout(words)          # a function in this module
same = words.shout           # the same call
```

Where a record's field and a function share a name, `x.name` is the field
where the field is visible, and the function elsewhere (section 10.2). When the
field holds a function, `x.name(a)` calls it with `a`.

`T.to_text(x)` is `x.to_text`, and `T.compare(a, b)` is `a.compare(b)`, for
any type with `Text` or `Order` (section 9.2), including one that has it
automatically. `T` is the type, as in `Text.compare(a, b)` or
`Option.to_text(found)`, a type parameter, or the module that declares the
type: `Shapes.to_text(shape)` takes any type `Shapes` declares. Without
arguments they are function values, as in `shapes.map(Shape.to_text)`.

### 5.5 Generic functions

A single capital letter in a signature is a type parameter, and `where` asks
for abilities (section 9):

```mo
fn largest(items: List(T)) : Option(T) where T: Order
  items.sort.last
end
```

A generic function can't call itself, directly or through other functions,
with a type parameter standing for a bigger type built from that parameter. If
`depth(t: Tree(T))` leads to a call of `depth` on a `Tree(List(T))`, `T` stands
for `List(T)` there, and that is an error. Calling `depth` on a node's `left`,
another `Tree(T)`, is fine. Each type a generic function is used with gets its
own copy of the function, and such a call would need copies without end: for
`Tree(List(T))`, `Tree(List(List(T)))`, and so on.

### 5.6 Functions as values

A named function is a value: `prices.map(with_tax)`. An anonymous function is
written `fn(x) ... end`, on one line or several, and its parameter types are
inferred. A function type is written like a signature: `fn(Decimal) : Decimal`,
or `fn(Text)` for one that returns `Unit`.

<!-- mo-example: fragment -->
```mo
taxed = prices.map(fn(p) p * 1.2 end)
labels = orders.map(fn(order)
  status = if order.paid?: "paid" else: "due"
  "#{order.id}: #{status}"
end)
```

An anonymous function can read names in scope where it is made, but it can't
assign a `var` and can't capture a capability (section 11.4), so it can do only
what its parameters allow; it may take a capability as a parameter. So
printing each item works in a `for` loop, not inside `map`.

An anonymous function may read a `var` from the function around it only if
nothing assigns that `var` after the anonymous function is made, including in
a later round of a loop around it. Reading any other `var` is an error, since
it would be unclear whether the function sees the value it was made with or a
later one. Copy the value into a name first, and read the name:

<!-- mo-example: fragment -->
```mo
var count = 0
current = count
show = fn() "#{current}" end  # fn() "#{count}" end would be an error
count += 1
console.print(show())  # prints 0
```

To use a later value, make the function after the last assignment, or pass
the value as a parameter.

`return` and `try` inside an anonymous function leave that anonymous function,
not the function around it.

## 6. Names, bindings and variables

### 6.1 Bindings

`name = value` binds a name once. A name is visible from its binding to the end
of its block: a function's or a test's body, a branch of an `if`, an arm of a
`case`, a loop's body, or an anonymous function's body.

Within a function or a test a name means one thing. While a local name, a parameter or
a bound name, is visible it can't be bound again, even in an inner block or an
anonymous function's parameters. So `line = line.trim` is an error: write
`trimmed = line.trim`, or use a `var`. Separate blocks, such as two arms of a
`case`, may use the same name. A function also can't refer to one of its
module's functions or constants by a name it uses for a local:
`fn open(owner: Text)` is fine in a module with an `owner` function, as long
as `open` doesn't call `owner(...)`.

Unused names, parameters and imports are errors, and so is a result that isn't
`Unit` and isn't used. A name starting with `_` is unused on purpose, and
`for _ in 0..3` repeats without a name.

<!-- mo-example: fragment -->
```mo
subtotal = order.lines.map(fn(line) line.price end).sum
tax = subtotal * 0.2
```

### 6.2 Variables

`var name = value` declares a name that can be assigned again within the
function that declares it. Assigning to a field, or to a field of a field,
builds a new record with that field changed:

<!-- mo-example: fragment -->
```mo
var cart = Cart(owner: "ana", items: [], total: 0)
cart.items = cart.items.append(item)
cart.total += item.price
```

`+=` and `-=` add to and subtract from a `var` or its fields; `mo format`
writes `x = x + y` as `x += y`. Assignment never changes a value anyone else
holds: whoever holds the old cart still sees the old cart. When the `var` is its
value's only holder, the new value reuses its memory.

No parameter is passed by reference: a function returns a new value, and its
caller assigns it.

## 7. Control flow and patterns

### 7.1 if

`if` runs a block when its condition is true, with optional `else if` and
`else` blocks. With an `else`, an `if` has a value: the value of the block that
ran. A block `if` stands on a line of its own or after `=`.

<!-- mo-example: fragment -->
```mo
if temperature > 30
  console.print("hot")
else if temperature < 5
  console.print("cold")
else
  console.print("mild")
end

grade = if score >= 90
  "A"
else
  "B"
end
```

A value that fits on one line uses the one-line form, which always has an
`else` and may chain:

<!-- mo-example: fragment -->
```mo
unit = if count == 1: "item" else: "items"
sign = if n < 0: "negative" else if n == 0: "zero" else: "positive"
```

### 7.2 case

`case` compares a value with patterns, top to bottom, and runs the first arm
that matches. An arm is `Pattern: statement` on one line, or `Pattern:` followed
by lines that run until the next arm or `end`. The value of a `case` is the
value of the arm that ran.

<!-- mo-example: fragment -->
```mo
message = case outcome
  Ok(order): "placed order #{order.id}"
  Error(UnknownItem(name: n)): "no such item: #{n}"
  Error(BadQuantity(problem)):
    detail = describe(problem)
    "bad quantity: #{detail}"
end
```

The arms must cover every possible value; if they don't, the error lists what's
missing. When the value is a choice type, a whole arm can't be a catch-all, `_`
or a name: list the cases, so that a case added later shows every place that
must handle it. Inside a pattern, `_` and names may stand for anything, as in
`Error(_)`. Arms with the same body share a line, as in `Saturday | Sunday: "weekend"`,
and an arm may have a guard, as in `Some(n) if n > 10: "many"`.

### 7.3 Patterns

| Pattern | Matches |
| --- | --- |
| `_` | anything, and ignores it |
| `name` | anything, and binds it to `name` |
| `42`, `-1`, `2.5`, `"GET"`, `'x'`, `true` | that literal value |
| `None`, `Some(p)`, `Circle(p)` | that case, with its value matching `p` |
| `Rectangle(width: p, height: q)` | that case or record, with the named fields matching; fields not named are ignored |
| `(p, q)` | a tuple |
| `[]`, `[p, q]`, `[first, ..rest]`, `[first, .._]` | an empty list, a list of exactly two, and a list of at least one, with or without a name for the rest |

A case with named fields is matched by naming them, even when it has only
one: `NotFound(path: p)`, or `NotFound(path: _)` to ignore the path. The form
`Circle(p)` is only for a case's one unnamed value.

### 7.4 is

`value is Pattern` tests a pattern, giving a `Bool`. The names the pattern
binds are visible where the test is known to be true: in the block of the `if`
or `while` it guards, after `and` in the same condition, after `implies`, and
after an `assert`.

<!-- mo-example: fragment -->
```mo
if reply is Ok(body) and not body.empty?
  console.print(body)
end
while console.read_line is Some(line)
  console.print(line.ascii_uppercase)
end
```

### 7.5 Loops

`for` runs a block for each element of a list, set, map or range, and `while`
runs a block while a condition is true. `break` leaves the loop, and `continue`
starts its next round. A range `a..b` counts from `a` up to, but not including,
`b`.

```mo
var total = 0
for i in 0..10
  continue if i % 2 == 0
  total += i
end

var remaining = 100
var halvings = 0
while remaining > 1
  remaining = remaining / 2
  halvings += 1
end
```

## 8. Failures and missing values

Mo has no null and no exceptions. A value that may be missing is an `Option`; a
function that can fail returns a `Result`; and a bug stops the program
(section 13.2). Both are ordinary choice types:

<!-- mo-example: fragment -->
```mo
enum Option(T)
  None
  Some(T)
end

enum Result(T, E)
  Ok(T)
  Error(E)
end
```

### 8.1 Options

`x or default` gives the value inside a `Some`, or the default for `None`, and
computes `default` only for `None`. `x.or_fail(e)` turns `None` into
`Error(e)`; it is a function, so `e` is computed even when `x` is a `Some`.
`case` and `is` handle both cases:

<!-- mo-example: fragment -->
```mo
city = address.city or "unknown"
case users.get(id)
  Some(user): console.print(user.name)
  None: console.print("no user #{id}")
end
```

### 8.2 Results and declared failures

A function that can fail declares its failures as a choice type, and returns a
`Result`:

```mo
enum QuantityError
  Empty
  NotANumber(text: Text)
  Negative(value: Int)
end

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

### 8.3 Passing failures upward: try

`try expr` takes the value out of an `Ok`, or returns the `Error` from the
function it is in. It works on a `Result` in a function that returns a
`Result`, and on an `Option` in a function that returns an `Option`. In a
function that returns neither, such as `main : Int` or a test, handle the
failure with `case`. When the failure types differ, `try` wraps the failure if
the function's failure type has exactly one case that holds it as its unnamed
value, as `BadQuantity(QuantityError)` does:

<!-- mo-example: fragment -->
```mo
enum OrderError
  BadQuantity(QuantityError)
  UnknownItem(name: Text)
end

fn parse_line(line: Text, catalog: Map(Text, Item)) : Result(OrderLine, OrderError)
  parts = line.split(",")
  name = try parts.get(0).or_fail(UnknownItem(name: line))
  quantity = try parse_quantity(parts.get(1) or "")  # a QuantityError becomes BadQuantity(...)
  item = try catalog.get(name).or_fail(UnknownItem(name: name))
  Ok(OrderLine(item: item, quantity: quantity))
end
```

With no such case, or more than one, `try` is an error that says which case to
add. Ignoring a `Result` is an error too: every failure is passed upward or
handled.

### 8.4 Can't happen

There is no "unwrap". Where a failure truly can't happen, the code says why,
with `stop`:

<!-- mo-example: fragment -->
```mo
rate = case rates.get(currency)
  Some(r): r
  None: stop "every currency was checked against the rate table at startup"
end
```

Reaching a `stop` is a bug: the program stops and reports the reason. A `stop`
fits wherever a value is expected, because control never comes back from it.
So does a block or arm that ends in `return`: it leaves the function instead
of giving a value.

## 9. Generics and abilities

### 9.1 Generic types

`List(T)`, `Map(K, V)` and your own types take type parameters:

```mo
struct Page(T)
  items: List(T)
  number: Int
end
```

A generic type can't hold itself at a bigger type built from its parameter, as
a `Tree(T)` with a field `children: List(Tree(List(T)))` would. Each type a
generic type is used with becomes a type of its own, and that one would need
types without end, as such a call would for a generic function (section 5.5).
`children: List(Tree(T))` is fine.

### 9.2 The five abilities

An ability is something a type can do. There are five, all built in:

| Ability | What it gives | Customized with |
| --- | --- | --- |
| `Equal` | `==` and `!=` | nothing: values are equal when their fields are |
| `Hash` | use as a map key or a set member | nothing: always structural |
| `Order` | `<`, `<=`, `>`, `>=`, `sort`, `min` and `max` | `fn compare(a: T, b: T) : Ordering` |
| `Text` | a text form, for `#{...}` and printing | `fn to_text(x: T) : Text` |
| `Number` | `+`, `-`, `*`, `/`, `%` and `sum` | nothing: only `Int`, `BigInt`, `Decimal` and `Float` have it; times and durations also add and subtract (3.9) |

<!-- mo-example: fragment -->
```mo
enum Ordering
  Less
  Equal
  Greater
end
```

A type with `Hash` or `Order` also has `Equal`. Ability names appear only after
`where`, so they don't clash with other names. Records and choice types get
`Equal`, `Hash`, `Order` and `Text` automatically whenever all their fields
have them. Equality compares field by field; maps compare their entries, and
sets their elements, in any order. Order compares fields in the order they are declared, and
cases in the order they are declared. Functions and capabilities have no
abilities. The built-in types have these:

| Type | `Equal` | `Hash` | `Order` | `Text` | `Number` |
| --- | --- | --- | --- | --- | --- |
| `Bool` | yes | yes | `false` first | yes | no |
| `Int`, `BigInt`, `Decimal` | yes | yes | yes | yes | yes |
| `Byte` | yes | yes | yes | yes | no |
| `Float` | IEEE: "not a number" equals nothing | no | yes, with "not a number" last | yes | yes |
| `Text`, `Char` | yes | yes | by character code | yes | no |
| `Bytes` | yes | yes | byte by byte | no | no |
| `Unit` | yes | yes | yes | yes | no |
| `Time`, `Duration` | yes | yes | yes | yes | no (but they add and subtract, 3.9) |
| `Range` | yes | yes | no | yes: `0..3` | no |
| tuples | when every part has it | same | same, part by part from the left | same | no |
| `List(T)` | when `T` has it | same | same, element by element; a list comes before any longer list that begins with it: `[1, 2] < [1, 2, 0]` | same | no |
| `Map(K, V)`, `Set(T)` | when their keys and values have it, comparing entries in any order | same | no | same | no |

`Option`, `Result` and `Ordering` are choice types, so their cases are ordered
as they are declared: `None`, `Some`; `Ok`, `Error`; `Less`, `Equal`,
`Greater`. `Float` follows IEEE rules: "not a number" equals nothing and sorts
last, and floats have no `Hash`, so they can't be map keys.

A value's text form looks like the code that builds it: `Point(x: 1.5, y: 2)`,
`Some(3)`, `[1, 2]`, `{"a": 1}`, `(1, true)`, `[1, 2].to_set`. A `Text` or
`Char` value's text form is itself; inside another value's, it is quoted and
escaped as in code. Numbers have their shortest spelling. A `Float` is written
in plain digits, as `1000` or `0.25`, unless its size is below 1e-6 or at
least 1e21, when it has an exponent, as `1e21` or `1.5e-7`. A whole float has
no `.0`, zero is `0` and negative zero `-0`, and a float that is not a number
is `nan`, the infinities `inf` and `-inf`. A `Bool` is `true`
or `false`, a `Time` is RFC 3339 text, and a `Duration` is in seconds, as
`1.5s`. `x.to_text` gives the text form of any value whose type has `Text`:
the same text as `"#{x}"`.

`x.compare(y)` compares two values whose type has `Order`, giving `Less`,
`Equal` or `Greater`.

A type customizes `Order` or `Text` with one ordinary function in its own
module, which applies wherever the type goes, exposed or not:

```mo
struct Money
  cents: Int
  currency: Text
end

fn to_text(money: Money) : Text
  "#{(money.cents.to_decimal / 100).to_fixed(2)} #{money.currency}"
end
```

There are no user-defined abilities in v0.1. Code that needs something other
than these five takes a function as a parameter.

## 10. Modules

### 10.1 The header

A module starts with its name, then the `expose` line listing its public names,
then its `use` lines:

<!-- mo-example: fragment -->
```mo
## Orders: building, pricing and checking them.
module Shop.Orders
expose Order, OrderError(..), place, total
use Shop.Catalog
use Shop.Money{Money, add}
```

Everything not on the `expose` line is private to the module. Tests in the same
file see everything (section 12.3).

### 10.2 Hidden insides

`expose Order` exposes the type without its fields or cases. Outside the module
an `Order` can't be built, taken apart or have its fields read; it can only be
passed to the module's functions. `expose OrderError(..)` exposes the fields or
cases too. Hidden insides keep values valid: if `place` is the only way to get
an `Order`, every `Order` has passed `place`'s checks.

### 10.3 use

`use Shop.Catalog` lets this module write `Catalog.find(...)` and
`Catalog.Item`. `use Shop.Money{Money, add}` also brings those names in
unqualified, and a choice type's exposed cases come with it. A module may
declare a type with its own name, as the module `Account` declares `Account`:
`Account.open(...)` calls the module's function, and `Account` in a type is the
type. There are no wildcards and no aliases. A clash between names is an error that suggests the
qualified form, and so is a cycle of modules that use each other. The
library's `Json` and `Unicode16` modules are used the same way. Every library
module reserves its name, so a program can't declare a module named `FileError`,
`Files`, `Json`, `List`, `Map`, `Option`, `ReadFiles`, `Result`, `Set`, `Text`,
`Time` or `Unicode16`. The built-in `Simulation` module also reserves its name.

### 10.4 Constants

A module may bind fixed values at its top level:

```mo
max_attempts = 3
default_currency = "EUR"
```

A constant's type comes from its value, which must decide it; for anything
else, such as a list of anonymous functions, write a function with a
signature. Constants are computed once, before any other code of the program
runs, in the order of their dependencies. A constant depends on the constants
it uses, directly or through the functions it calls. So a constant may use one
written below it, or one in another module, but constants that depend on each
other in a cycle are an error. A constant that stops stops the program before
it starts. Module-level bindings are constants, not mutable state. A service
declares its state separately (section 11.5); handlers receive that state as
an argument and return any replacement state.

### 10.5 Always in scope

Always in scope: the built-in types of section 3, `Option` with `Some` and
`None`, `Result` with `Ok` and `Error`, `Ordering` with its cases, the
capability types, `FileError` with its cases, `SimulationError` with `ServiceDown` and `ToolFailed`,
and `Entry` with `EntryKind` and its cases. The functions of section 14 belong to their types' modules:
call them with a dot, as in `text.lines`, or through the module, as in
`Set.empty` and `Time.parse(text)`. So a local may be called `lines` or
`count` without clashing with them.

## 11. Capabilities and effects

### 11.1 What a function can touch

A function can affect or observe the world outside the program only through
capabilities it is handed as parameters. A capability's type names what it can
do: `Console` prints, `Files` reads and writes files, `Clock` tells the time.
So a function's parameter types are its complete list of effects, and a
function with no parameter that is or holds a capability only computes: given
the same arguments, it always gives the same result.

| Capability | Lets a program |
| --- | --- |
| `Args` | read its command-line arguments |
| `Env` | read environment variables |
| `Console` | print to standard output and standard error, and read standard input |
| `Files` | read and write files in one folder and the folders inside it |
| `ReadFiles` | only read them |
| `Clock` | read the current time |
| `Random` | draw random numbers |

### 11.2 main

`main` lists the capabilities the program needs, in any order and at most one
of each type. The runtime hands over exactly those, so a program's whole
authority is on its first line. The `Files` or `ReadFiles` it gets reach the
folder the program was started in.

`main` returns nothing, and the program exits with status 0; or an `Int`, the
exit status, from 0 to 255; or `Result(Unit, E)`, where `Error(e)` prints `e`'s
text form to standard error and exits with status 1.

```mo
## Prints the first line of each file named on the command line.
module Head

fn main(args: Args, console: Console, files: ReadFiles) : Result(Unit, FileError)
  for name in args.list
    text = try files.read_text(name)
    console.print(text.lines.first or "")
  end
  Ok(Unit)
end
```

### 11.3 Passing less

A capability can be narrowed before it is passed on. `files.within("reports")`
reaches only that folder, and `files.read_only` returns a `ReadFiles`, which
can only read. Calling `within` with a path that would leave the folder, such
as `".."`, gives a capability that reaches nothing (section 14). Pass each
function the least it needs:

<!-- mo-example: fragment -->
```mo
summary = try summarize(files.within("reports").read_only)
```

### 11.4 Rules

- Records may hold capabilities, so a function that needs several can take one
  record.
- Anonymous functions can't capture capabilities, or values holding them. A
  function that needs one takes it as a parameter.
- Only the runtime makes real capabilities. Tests, examples and promises use
  fakes (section 12.4).
- Capabilities have no abilities: they can't be compared, printed or used as
  keys.
- There are no log statements. To report something, print it or return it.

### 11.5 Service blocks

This build checks service blocks. `mo run` of a service entry module still reports E1408; see the end of this section.

A module may declare one top-level `service ... end` block. Its clauses name
the state type and start value, local functions offered as tools, timer
handlers, and the environment variable holding its access token.
`service` is reserved. The clause words `state`, `tools`, `every`, `token`,
`from` and `env` remain ordinary identifiers outside their clause positions.

Clauses occur in this order: exactly one `state`, exactly one `tools` naming
at least one function, zero or more `every` clauses, and exactly one `token`
last. Empty services and timer-only services are refused. No function name
may appear twice anywhere in the block, including across tools and timers.

```mo
module Counter

service
  state Int = 0
  tools increment, snapshots
  every 1.minute tick
  token from env "COUNTER_TOKEN"
end

fn increment(state: Int, _now: Time, amount: Int) : (Int, Bool)
  (state + amount, true)
end

fn snapshots(state: Int, _now: Time) : List(Int)
  [state]
end

fn tick(state: Int, _now: Time) : Int
  state + 1
end
```

The start value has the declared state type. The runtime uses it only when
there is no saved state. Each tool is a function declared in this module
whose first two parameters are the state type and `Time`, the moment the message arrived.
The service selects the local overload whose first parameter exactly matches
its state type, then requires that signature to contain no unresolved generic
parameters. Generic helpers can be called through concrete wrappers. A service
block has no explicit generic-specialization syntax. The state, handler inputs
and handler results must contain neither capabilities nor function values,
including inside records, choices, tuples and collections.

Tool names cannot end in `?` (E0106). Arguments after State and Time, the reply
and any application error must have visible fields and cases, including nested
types (E0105). Imported wire types need their representation exposed with `(..)`;
types declared in this module are visible locally. Private service state need
not be exposed. These wire checks run after the bare-state result check E0101.

A tool updates stored state only when it returns `(State, Reply)` or
`Result((State, Reply), E)`. Returning exactly `State`, or `Result(State, E)`, is an error.
For a bare `State` result, the error offers two fixes: return `(state, Unit)` to
update state, or wrap the value in a record to return it as read-only data.
`Result(State, E)` receives no fixes. `mo fix` applies neither alternative.
The suggested adapters retain the tool's name and `##` comment, rename the
original function to a helper, and rebind the module's references to that
helper. A function on the module's `expose` line receives no fixes, because a
fix can't update callers in other modules. Nor does a function whose name ends
in `?`: it must return `Bool` (section 2.3), so neither adapter could keep its name.
Every other result shape is a read-only reply,
including `List(State)`, `Option(State)` and records containing state.
Read-only messages are neither logged nor saved.

Each `every` clause has a duration literal with a whole-number value of at
least 1, such as `1.minute` or `30.seconds`, and a local handler with exactly
the signature `(State, Time): State`. Simultaneous ticks follow the source
order of the `every` clauses. Unlike a tool returning bare state, a timer
handler returns replacement state. Handlers remain ordinary functions that
examples, promises and tests can call directly.

The token source is a plain text literal naming an environment variable. The
name is nonempty and contains only uppercase ASCII letters, digits and `_`,
and cannot start with a digit. Checking does not read that variable or run a
service. This build checks, tests, formats and fixes service modules, but
`mo run` of a service entry module reports E1408, "mo run does not start
services yet". Importing a service module does not start it.

The private C embedding can expose a service through authenticated MCP over
plain HTTP/1.1. It provides no new Mo function, capability or command. A tool's
`structuredContent` is an object with `value` on success or `error` on failure;
one text-content block contains the same compact object. Runtime errors report
an effect of `none`, `applied` or `unknown` on this call's service state. An effect
does not promise retry safety; an updating call whose response is lost has an
unknown effect from the client's perspective.

## 12. Examples, promises and tests

A function's specification sits at the top of its body: its promises and
examples, then a blank line, then its code. Leaving out the blank line is not
an error; `mo format` writes it.

### 12.1 Examples

`examples` lists calls with their expected results. Each line is a `Bool`
expression, usually `call == expected`, and `mo test` checks every line:

```mo
fn initials(name: Text) : Text
  examples
    initials("Ada Lovelace") == "AL"
    initials("") == ""
  end

  firsts = name.words.filter_map(fn(word) word.chars.first end)
  Text.from_chars(firsts)
end
```

### 12.2 Promises

A `promise` states what must hold for every call. It has three parts:

- **a sentence** saying, in plain words, what is promised;
- **a body**: one or more `Bool` lines over the parameters and `result`, the
  function's result, which must all be true;
- **anchors**: at least one `allows` line and one `forbids` line. Each gives
  every parameter and `result` a value, which the body must accept or reject.

```mo
fn ordered(a: Int, b: Int) : (Int, Int)
  promise "the smaller comes first, and both values are kept"
    result.0 <= result.1
    result == (a, b) or result == (b, a)
    allows a: 5, b: 2, result: (2, 5)
    forbids a: 5, b: 2, result: (2, 2)
  end

  if a <= b: (a, b) else: (b, a)
end
```

The compiler checks that the body accepts every `allows` and rejects every
`forbids`, which catches a body that doesn't say what its sentence says.
`mo test` checks the body against the function on many generated inputs,
boundaries first. It builds a type exposed without its insides only through a
limited number of calls to its module's exposed functions, other types from any
values of their fields, and capabilities as fakes. A value built that way can
come with another such value that the same call returned, so a `Lease` can
arrive with the `Queue` that issued it. A promise speaks about calls
that return; a call that stops on a generated input is reported as a bug, with
the input. The boundaries include each type's extremes, such as `Decimal`'s
largest value and the last instant of the year 9999, so a promise that passes
holds for every call its types allow. When an extreme stops a call, the report
says so, as in "stops with overflow when `amount` is `Decimal`'s largest
value", and shows the two fixes: return a failure for such inputs, or take a
type whose module keeps its values in range. Proofs come in a later version.

After a false promise, `mo test` tries smaller inputs, keeping only those that
still fail. For a hidden type, it shortens the recorded builder sequence and
simplifies the arguments to those calls; it never edits hidden fields. Values
used as same-call companions stay whole. Builder reduction and other input
shrinking share at most 200 additional subject calls and the original deadline;
the reported counterexample need not be the smallest possible one.

Recording is optional and bounded to 4096 builder recipes per generated tuple,
including recipes made during unsuccessful starts. Exhaustion does not change
which inputs can be generated or whether they pass. If that tuple fails with
incomplete recording, the report keeps the original failing input and adds
E1404 explaining that shrinking is limited.

For a generic function, `mo test` checks every concrete instantiation already
present in the program, including those supplied by anchors, examples and tests.
It keeps each complete list of type arguments as used; it does not combine
arguments from different uses or choose additional types. It generates and
shrinks values separately for each instance, and a failure names its concrete
types. This checks those instances, not every possible substitution for the
function's type parameters.

A promise about many steps, such as "an accepted job is never lost", is a
promise on the function that takes one step: something each step keeps true
between its input and its result.

There are no preconditions. What a caller must guarantee becomes a type, such
as a `Quantity` that its module never builds negative, or a failure, such as
`Error(InvalidAmount)`.

### 12.3 Tests

A `test` names a scenario in a sentence and checks it with `assert`. Tests
live in the same file as the code and see its private names. A name bound by
`assert ... is Pattern` stays in scope for the rest of its block:

<!-- mo-example: fragment -->
```mo
test "a quantity is read past spaces"
  assert parse_quantity(" 12 ") is Ok(n)
  assert n == 12
  assert parse_quantity("-1") == Error(Negative(value: -1))
end
```

### 12.4 Fakes

Tests, examples and promises make fake capabilities, which reach nothing
outside the program: `Console.fake()`, whose `printed` and `printed_errors` list
the lines written to standard output and standard error;
`Console.fake_input(lines)`, which also has those lines to read;
`Files.in_memory(files)`, from a map of paths to texts; `Clock.fixed(time)`;
`Random.seeded(n)`; `Args.of(list)`; `Env.of(map)`; and `Simulation.start(module, at)`
for a test service. Fakes are made only
there, never in a module's constants or its functions. `printed` and
`printed_errors` can be used only there too, where every console is a fake,
and using them anywhere else is an error.

`Simulation.start(Module, at: Time) : Simulation` starts the selected service
with an in-memory store and runs its startup ticks. `Simulation.send(handle,
Module.tool, args...)` supplies state and the current fake time, then returns
`Result(R, SimulationError)` for the tool's complete declared result R. Only
direct module and registered tool selectors are allowed, including registered
tools not listed in `expose`. Timers, other private functions and function
values are not selectors. Arguments after the selector exactly match the
tool's parameters after State and Time. An application `Error(e)` is returned
as `Ok(Error(e))`; it does not count toward the failure limit. `ServiceDown` means
nothing was queued, and `ToolFailed` means a handler defect or work/memory
limit, with no update committed. A state-changing `Ok(r)` means committed.
Each `ToolFailed` counts toward the service's failure limit: 5 within 60 seconds
of simulated time restart the service from its last saved state, and 3 such
restarts within 10 minutes stop it, after which `send` returns `ServiceDown` until `restart`.
Call these operations through the module, as in `Simulation.crash(sim)`; dot
calls such as `sim.crash` are not available.

`Simulation.advance(handle, by: Duration)`, `Simulation.crash(handle)` and
`Simulation.restart(handle)` return Unit. Advance visits each deadline in
order, supplies that deadline as `now`, and drains simultaneous timers in
service source order. Zero does nothing. A crashed service's clock advances
without running handlers. Before advancing a running service, all due firings
are counted separately; more than 10,000 is a bug with no clock movement,
execution or commit. Negative advances and times beyond year 9999 are bugs.
Each successful operation returns with an empty mailbox. Send is synchronous
and cannot be interrupted by crash.

Crash is idempotent. It discards live state, timers and supervisor history,
but retains the same open store and clock. Restart recovers through the real
log and snapshots, emits no replies for replay, and starts fresh supervision
and startup ticks. Restarting a running service or selecting another service's
tool is a bug. Copies of the capability share its identity; its last release
closes the service and then the store. Helpers holding the handle may use all
operations except start. Like other capabilities, Simulation cannot be stored
in service state or tool signatures, and cannot be a main parameter.

Startup, timer, persistence and invalid-recovery failures stop the test with
a bug report, not a SimulationError. Arguments and replies are copied into and
out of the service, limited to 128 levels of nesting, 1,048,576 values and 16 MiB encoded, including
read-only and application Error replies. Copy-in failure queues and commits
nothing. Updating messages also obey the aggregate saved-message byte limit;
an admission refusal reports that no handler ran and does not count toward the
failure limit. Read-only messages are not saved. Copy-out failure names the
executed tool and any committed update; it does not roll back or retry.
Promise and generator support is deferred.

A capability is a handle on something outside the program, and a fake is a
handle on something kept for the test. Every use of one fake sees the same
files and the same output: a file that `main` writes to a
`Files.in_memory(...)` fake can be read back, and a `Console.fake()` passed to
`main` shows afterwards what `main` printed.

<!-- mo-example: fragment -->
```mo
test "head prints the first line of each file"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one\ntwo\n"}).read_only
  assert main(Args.of(["a.txt"]), console, files) == Ok(Unit)
  assert console.printed == ["one"]
end
```

## 13. Running

### 13.1 Evaluation

Mo evaluates eagerly, left to right: a call's arguments, then the call; an
operator's left side, then its right. `and`, `or` and `implies` skip their right
side when the left decides the answer, and `x or default` on an `Option`
computes `default` only when `x` is `None`. `try` returns early, and `stop`
stops the program.

### 13.2 Bugs

A bug stops the program at once, with a report of what happened and where, and
exit status 70. These are bugs: overflow in `Int`, `Decimal` or `Duration`
arithmetic, or a time outside the years 1 to 9999; dividing by zero; reaching a `stop`;
using more stack, call depth or memory than section 13.4's limits allow, or
more memory than the machine has; calling a library function outside what it
accepts, such as `take(-1)` or `random.int(5, 5)`; two keys of a map literal
that turn out equal (section 3.8); an exit status outside 0 to 255; printing to
an output that is gone; having too many folders open at once; and a fake whose
files make one path both a file and a folder (section 14). Bugs are never
caught.

### 13.3 Costs

Mo promises what operations cost, not how values are stored:

| Operation | Cost |
| --- | --- |
| `get`, `size`, `first` and `last` on a list | constant |
| `append` on a list, and assigning a `var`'s field | constant when the value has one holder; a copy when it's shared |
| `get` on a map, and `has?` on a map or a set | constant on average |
| `set` and `remove` on a map, and `add` and `remove` on a set | constant on average when the value has one holder; a copy when it's shared |
| `byte_count` | constant |
| `char_count`, `split`, `lines`, `contains?` | proportional to the text's length |
| `sort`, `sort_by` and `sort_by_desc` | n log n, and stable: equal keys keep their order |

A value is shared when two names, fields or collections hold it. Changing a
value with one holder reuses its memory; changing a shared value copies it
first, and its other holders keep the old value.

### 13.4 The `mo` command

Each command is given the file of the program's main module; `mo check` and `mo
test` also accept a library module. `mo check FILE`
reports the program's errors, `mo run FILE [ARGS...]` runs it with the
arguments after the file's name, and `mo test FILE` checks its examples,
promises and tests. `mo emit FILE -o OUT` checks the program as `mo check` does
and writes its bytecode to `OUT`; `-o` is required, and a library module has no
main to emit (E0079). `mo format` and `mo fix` rewrite the files they are given.
`mo guide` needs no file. It prints a short guide for agents writing Mo,
matched to the installed version: how to work with the `mo` command, a small
complete program with its tests, and when a task is done. `mo guide
--reference` prints this reference.
`mo` exits with status 0 when everything passed, 1 when the program has errors
or an example, promise or test fails, and 2 for a mistake in the command
itself. Once the program runs, `mo run` exits with the program's own status
(section 11.2), or 70 after a bug (section 13.2).

`mo test` runs every example, promise and test unless it's told which. Each has
a full name: its function's name, or a test's module's, then its kind and a
number counted from 1 in source order, such as `Wc.count.example.3` for the
third example of `Wc.count`, `Account.transfer.promise.1`, or `Wc.test.2` for
the second test in `Wc`. `--only NAME` runs the one with that full name, and can be given more
than once; `--in MODULE` runs one module's, still checking the whole program;
and `--list` prints the full names without running anything. A selection that
matches nothing is a mistake in the command.

`mo run` and `mo test` limit what a program uses: `--max-stack` sets the bytes
of stack, by default 8 MiB; `--max-depth` the calls in progress at once, by
default 10,000; and `--max-heap` the bytes of memory, by default 1 GiB. Going
past one is a bug whose report names the limit (section 13.2). A call in tail
position takes its caller's place on the stack and doesn't count toward the
depth (section 5.3). A program can't set its own limits.

`mo check --json` and `mo test --json` write one JSON object per line to
standard output: first the format's version, then one for each error and for
each example, promise and test run, and last the totals and the exit status.
Within a major version of the format, every field keeps its name, type and
meaning, and an error's code keeps its meaning; new fields and new kinds of
object may be added, so a reader skips what it doesn't know. A message's
wording may change.

## 14. The core library

Each function is listed under the type of its first parameter, written as it
is called with a dot: under **Text**, `split(separator: Text) : List(Text)` is
called as `text.split(",")`. A function written `Type.name(...)` is called
through the type's module. `pred` is a function returning a `Bool`. Calling a
function outside what it accepts, such as `take(-1)`, is a bug (section 13.2).
Every type with the `Text` ability also has `to_text : Text`, its text form,
and every type with `Order` has `compare(other: T) : Ordering` (section 9.2).

**Int**: `abs`, `min(b: Int)`, `max(b: Int)`, `pow(n: Int)` (`n` at least 0),
`floor_div(b: Int)`, `floor_mod(b: Int)`, and `wrapping_add`, `wrapping_sub`,
`wrapping_mul`, `saturating_add`, `saturating_sub` and `saturating_mul` (each
taking `b: Int`), all `: Int`. `checked_add`, `checked_sub` and `checked_mul`
`(b: Int) : Option(Int)`. `to_decimal : Decimal`, `to_float : Float` (rounded
to the nearest float), `to_big : BigInt`, `to_byte : Option(Byte)`,
`to_char : Option(Char)`, `to_text : Text`. `seconds`, `milliseconds`,
`minutes`, `hours` and `days : Duration`, also named `second`, `millisecond`,
`minute`, `hour` and `day` respectively (3.9).

**BigInt**: the arithmetic operators, `abs`, `min(b: BigInt)`, `max(b: BigInt)`,
`pow(n: Int)`, `floor_div(b: BigInt)` and `floor_mod(b: BigInt)`, all
`: BigInt`; `to_int : Option(Int)`, `to_text : Text`. **Byte**:
`to_int : Int`, `to_text : Text`.

**Decimal**: `abs`, `min(b: Decimal)`, `max(b: Decimal)`, `round(places: Int)`
(ties away from zero), `floor` and `ceil`, all `: Decimal`.
`to_int : Option(Int)` (`None` unless whole and in range), `to_float : Float`
(rounded to the nearest float), `to_fixed(places: Int) : Text` (rounded as
`round` does), `to_text : Text`. `places` is from 0 to 18.

**Float**: `abs`, `floor`, `ceil`, `round` (ties away from zero) and `sqrt`,
all `: Float`; `nan?` and `infinite? : Bool`; `to_decimal : Option(Decimal)`:
the number the float's text form writes, rounded to 18 places with ties to
even, so `0.1.to_decimal` is `Some(0.1)`, and `None` for not-a-number, the
infinities and anything beyond `Decimal`'s range; `to_text : Text`.

**Text**: `byte_count : Int`, `char_count : Int`, `empty? : Bool`,
`chars : List(Char)`, `bytes : Bytes`. `lines : List(Text)`: split where `\n`
or `\r\n` ends a line, without the line endings; `""` has none, a final line
break doesn't start another line, and a `\r` alone stays in its line.
`words : List(Text)`: split at ASCII white space, which alone gives none.
ASCII white space is space, tab, line feed, vertical tab, form feed and
carriage return. `split(separator: Text) : List(Text)`: the separator isn't
empty, and empty pieces are kept, so `"".split(",")` is `[""]`. `trim`,
`trim_start` and `trim_end : Text` (ASCII white space).
`starts_with?(prefix: Text)`, `ends_with?(suffix: Text)` and
`contains?(part: Text) : Bool`. `drop_prefix(prefix: Text)` and
`drop_suffix(suffix: Text) : Option(Text)`: `None` when the text doesn't have
it. `replace(old: Text, new: Text)` (`old` isn't empty), `repeat(n: Int)` (`n`
is at least 0), `pad_left(width: Int)` and `pad_right(width: Int)`, all
`: Text`; padding adds spaces, counting characters, and leaves text that is
already as wide, or wider, unchanged, even for a width below 0.
`ascii_lowercase` and `ascii_uppercase : Text`. `to_int : Option(Int)`: an
optional `-` and ASCII digits, in the `Int` range, and nothing else.
`to_decimal : Option(Decimal)`: an optional `-`, one or more ASCII digits, then
optionally `.` and 1 to 18 digits, in the `Decimal` range, and nothing else, so
`"1."` and `".5"` give `None`.
`to_float : Option(Float)`: an optional `-`, one or more ASCII digits,
optionally `.` and one or more digits, and optionally `e` or `E`, an optional
sign and one or more digits; or exactly `nan`, `inf` or `-inf`, the text forms
of section 9.2. It gives the nearest float, and `None` for anything else or for
a number too large to be a finite `Float`. `Text.from_chars(chars: List(Char)) : Text`.

**Char**: `code : Int`, `ascii_digit?`, `ascii_letter?` and
`ascii_white_space? : Bool`, `ascii_lowercase` and `ascii_uppercase : Char`,
`to_text : Text`.

**Bytes**: `size : Int`, `get(i: Int) : Option(Byte)`,
`decode_utf8 : Option(Text)`.

**List(T)**: `size : Int`, `empty? : Bool`. `get(i: Int)`, `first` and
`last : Option(T)`; `get` gives `None` for a position below 0 or past the end.
`append(x: T)`, `prepend(x: T)`, `concat(other: List(T))`, `reverse`,
`take(n: Int)` and `drop(n: Int)`, all `: List(T)`; `n` is at least 0, and
past the end they take or drop everything. `set(i: Int, x: T)` and
`remove_at(i: Int) : Option(List(T))`: `None` for a position that isn't there.
`map(f: fn(T) : U) : List(U)`, `filter(pred) : List(T)`,
`filter_map(f: fn(T) : Option(U)) : List(U)` (the values in the `Some`s),
`flat_map(f: fn(T) : List(U)) : List(U)`, `reduce(start: A, f: fn(A, T) : A) : A`
(from the left), `find(pred) : Option(T)`, `any?(pred)` and `all?(pred) : Bool`,
`count(pred) : Int`, `enumerate : List((Int, T))` (from 0),
`zip(other: List(U)) : List((T, U))` (as long as the shorter list).
`sort_by(key: fn(T) : K)` and `sort_by_desc(key: fn(T) : K) : List(T)` where
`K: Order`. `contains?(x: T) : Bool` where `T: Equal`. `sort : List(T)`, and
`min` and `max : Option(T)` (the first of equal elements), where `T: Order`.
`unique : List(T)`, `to_set : Set(T)` and
`group_by(key: fn(T) : K) : Map(K, List(T))` (keys in the order they first
appear), where `T` or `K` has `Hash`. `sum : T` where `T: Number` (`0` for an
empty list). On a `List(Text)`,
`join(separator: Text) : Text`; on a `List((K, V))`, `to_map : Map(K, V)`, in
which a key given more than once gets its last value and keeps its first
place, as with `set`.

**Map(K, V)**: `size : Int`, `empty? : Bool`, `get(key: K) : Option(V)`,
`has?(key: K) : Bool`, `set(key: K, value: V)`, `remove(key: K)` and
`update(key: K, default: V, f: fn(V) : V)`, all `: Map(K, V)`; `update` sets
the key to `f` of its value, or of `default` when it has none.
`keys : List(K)`, `values : List(V)` and `entries : List((K, V))`, in insertion
order.

**Set(T)**: `size : Int`, `empty? : Bool`, `has?(x: T) : Bool`; `add(x: T)`,
`remove(x: T)`, `union(other: Set(T))`, `intersection(other: Set(T))` and
`difference(other: Set(T))`, all `: Set(T)`; `to_list : List(T)`. `Set.empty`
is a constant: the empty set.

**Range**: `for` loops over it; `to_list : List(Int)`, `contains?(n: Int) : Bool`.

**Option(T)**: `x or default` (an operator) gives a `T`;
`or_fail(e: E) : Result(T, E)`; `map(f: fn(T) : U) : Option(U)`.

**Result(T, E)**: `map(f: fn(T) : U) : Result(U, E)`,
`map_error(f: fn(E) : F) : Result(T, F)`.

**Time**: `checked_add(d: Duration) : Option(Time)`, `to_text : Text`, and the
operators of section 3.9. `to_text` writes RFC 3339 in UTC, with `Z`, and a
fraction of a second only when there is one, as short as it can be:
`2026-10-12T02:15:00Z`, `2026-10-12T02:15:00.5Z`.
`Time.parse(text: Text) : Option(Time)` reads RFC 3339 text, such as
`2026-10-12T04:15:00.5+02:00`: `T` or `t` between the date and the time, at
most 6 digits of a fraction of a second, then `Z`, `z` or an offset, which it
converts to UTC. Anything else, including a 7th fraction digit or a leap
second (second 60), gives `None`. **Duration**: `in_milliseconds : Int` (rounded toward
zero), `in_seconds : Decimal` (exact), `to_text : Text`.

**Args**: `list : List(Text)`, the arguments after the program's name.
**Env**: `get(name: Text) : Option(Text)`.

**Console**: `print(text: Text)` and `error(text: Text)` write a line to
standard output or standard error; print other values with interpolation, as
in `console.print("#{count}")`. `read_line : Option(Text)` reads a line from
standard input, without its line ending, `\n` or `\r\n`, and gives `None` at
the input's end. Bytes that aren't valid UTF-8 become U+FFFD, the replacement
character, one for each maximal invalid subpart, as Unicode recommends: the start of a
valid sequence that ends too soon, or else a single byte. The
bytes `a`, FF, FE, `b` read as `a`, two U+FFFD, `b`. A fake also has `printed` and
`printed_errors : List(Text)`, with one entry for each line written. Using them
outside tests, examples and promises is an error (section 12.4).
`Console.fake() : Console`, `Console.fake_input(lines: List(Text)) : Console`.

**Files**: `read_text(path: Text) : Result(Text, FileError)`,
`read_bytes(path: Text) : Result(Bytes, FileError)`,
`write_text(path: Text, text: Text)` (replaces the whole file),
`append_text(path: Text, text: Text)`, `remove(path: Text)` and
`make_folder(folder: Text)`, each `: Result(Unit, FileError)`; `list(folder: Text) : Result(List(Text), FileError)`,
the names in the folder, sorted; `exists?(path: Text) : Bool`;
`within(folder: Text) : Files`; `read_only : ReadFiles`. Writing or
appending into a folder that doesn't exist gives `Error(NotFound(path: path))`;
`make_folder` makes the folder and any missing folders above it, and succeeds
if the folder already exists.
`entries(folder: Text) : Result(List(Entry), FileError)` gives the folder's
entries in `list`'s order, each an `Entry(name: Text, kind: EntryKind)` whose
kind is `File`, `Folder`, `Link` or `Other`; a link is reported, never
followed. `walk(folder: Text) : Result(List(Entry), FileError)` gives every
entry below the folder, each named by its path from the folder: a folder's
entries in `list`'s order, each folder followed by what's inside it. It doesn't
enter links, and stops at the first folder it can't read. A capability from
`within` or `read_only` holds its folder open while it's used, and having too
many open at once is a bug (13.2).
`Files.in_memory(files: Map(Text, Text)) : Files`. In such a fake, the
folders are those its files are in, those `make_folder` made, and those whose
last file was removed, as on a disk, and `list` and `entries` give the names
directly inside a folder, each subfolder once. Files that make one path both a
file and a folder, as `{"a": "x", "a/b": "y"}` do, are a bug (13.2).
So are keys that aren't plain relative paths, such as `"/a"`, `"./a"`, `"a/"` or `"a//b"`.
**ReadFiles** has `read_text`,
`read_bytes`, `list`, `entries`, `walk`, `exists?` and `within`. Paths are relative and use `/`; `"."` is the capability's own
folder, and no path can leave it. Given a path that would leave the folder,
such as `"../a.txt"`, reading, writing, removing and listing give
`Error(NotAllowed(path: path))`, and `exists?` gives `false`. Calling `within` with
such a path gives a capability that reaches nothing: every call on it that
returns a `Result` gives `Error(NotAllowed(path: path))`, and `exists?` on it gives
`false`. A path may pass through symbolic links that stay inside the folder,
and every operation follows them; a path through an absolute link, or one that
leads outside, is refused in the same way, and one through more than 40 links
gives `Error(Failed(path: path, reason: "too many links"))`. `remove` removes a
link itself, never what it points to. Listing a file, or making a folder where a file is,
gives `Error(Failed(path: path, reason: "not a folder"))`, and reading, writing,
appending to or removing a folder gives
`Error(Failed(path: path, reason: "is a folder"))`.

**FileError**: `NotFound(path: Text)`, `NotAllowed(path: Text)`,
`NotText(path: Text)`, when `read_text` finds a file that isn't valid UTF-8,
and `Failed(path: Text, reason: Text)`. Its text forms are
`PATH: no such file`, `PATH: not allowed`, `PATH: not text` and
`PATH: REASON`, as in `notes/a.md: no such file`.

**Clock**: `now : Time`, a bug if the system's clock is outside the years 1 to
9999; `Clock.fixed(time: Time) : Clock`. **Random**:
`int(low: Int, high: Int) : Int`, from `low` up to, but not including, `high`,
which must be greater; `Random.seeded(seed: Int) : Random`. **Args** and
**Env** fakes: `Args.of(list: List(Text))`, `Env.of(map: Map(Text, Text))`.

**Json**, with `use Json`: `Json.parse(text: Text) : Result(Json, JsonError)`
reads JSON as RFC 8259 defines it, into this type:

<!-- mo-example: fragment -->
```mo
enum Json
  Null
  Boolean(Bool)
  Number(JsonNumber)
  String(Text)
  Array(List(Json))
  Object(Map(Text, Json))
end
```

An object keeps its keys in the document's order, and a key given twice is an
error. A `JsonNumber` keeps the number as it was written, and two are equal
when they're written the same: its `to_int : Option(Int)`,
`to_decimal : Option(Decimal)` and `to_float : Option(Float)` each give `None`
when the number doesn't fit, and `to_text : Text` gives it as written.
`to_decimal` never rounds: after the exponent and trailing zeros are applied,
a value that needs more than 18 decimal places gives `None`, so `1e-19` gives
`None` and `0.10` and `1.0e0` convert.
`Json.int(n: Int)` and `Json.decimal(d: Decimal)` make a number, and
`Json.float(f: Float) : Option(Json)` gives `None` for not-a-number and the
infinities, which JSON can't write. `json.to_text` writes compact JSON, and
`json.pretty` indents it by two spaces; both keep each object's order and each
number's text. A `JsonError(line: Int, column: Int, problem: JsonProblem)` says
where reading stopped, counting both from 1, and why: `UnexpectedEnd`,
`Unexpected(found: Char)`, `BadNumber`, `BadEscape` (including a lone surrogate
in a `\u` escape), `DuplicateKey(key: Text)`, or `TooDeep`, for arrays and
objects nested more than 128 deep.

**Unicode16**, following Unicode 16.0: `graphemes(text: Text) : List(Text)`,
`display_width(text: Text) : Int`, and `normalize_nfc`, `lowercase` and
`uppercase(text: Text) : Text`. `letter?(c: Char) : Bool` tests General_Category
L (`Lu`, `Ll`, `Lt`, `Lm`, `Lo`); `digit?(c: Char) : Bool` tests General_Category
`Nd` (decimal digits). Neither includes `Nl`, `No` or marks, and neither
normalizes or uses locale tailoring. These are code-point predicates, not
tests of the derived Alphabetic property or whole grapheme clusters.
`display_width` counts each user-perceived
character, as `graphemes` splits the text: 2 columns for an emoji (with its
selector, skin tone or joins), a flag, or one holding a wide or fullwidth
character; 0 for one made only of control characters, marks and invisible
characters; and 1 otherwise, ambiguous characters and a lone flag letter
included. A later runtime adds `Unicode17` beside it, so what a program computes
changes only when its `use` line does.

## 15. What comes later

Processes, messages and supervision, and tool schemas derived from types
(Phase 3); runtime introspection and replay (Phase 4); live upgrades
(Phase 5); approving and locking promises (Phase 6); demanding that a function
update its input in place (Phase 7); packages and user-defined abilities
(Phase 8). None of them changes a rule in this reference.

## Appendix A. Errors

Every error has a headline in this reference's words, the code with the exact
spot marked, one reason citing the rule's section, and the fix:

```text
error E0412: a failure is ignored                        shop/orders.mo:14:3

   14 │   files.write_text("orders.txt", report)
      │   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ returns Result(Unit, FileError), and the result is dropped

Mo never drops a failure silently (reference 8.3). Pass it upward, or handle it:

Fix:
   14 │   try files.write_text("orders.txt", report)
```

A code such as `E0412` never changes its meaning. `mo check --json` gives each
error as JSON (section 13.4): its code, headline and reason, the section the
reason cites, the places it marks, as byte offsets and as lines and columns
counted in characters, and its fixes, each one or more text edits applied
together. `mo fix` applies a fix when there is only one. Errors are reported in
source order, without the follow-on errors one mistake causes. There are no
warnings: a problem is an error, or it isn't reported.

## Appendix B. Grammar

The executable grammar, `grammar/mo.y`, is the authority, and this is its
summary. `x*` means zero or more, `x+` one or more, `x?` optional, and `NL` a
line break as section 2.4 defines it.
Section 11.5 fixes the order and number of a service's clauses.

```text
module      = "module" Path NL ("expose" exposed ("," exposed)* NL)? use* item*
exposed     = name | Name | Name "(" ".." ")"
use         = "use" Path ("{" (name | Name) ("," (name | Name))* "}")? NL
item        = function | struct | enum | test | service | name "=" expr NL
service     = "service" NL ("state" type "=" expr NL | "tools" name ("," name)* NL | "every" expr name NL | "token" "from" "env" text NL)* "end" NL
function    = "fn" name "(" params? ")" (":" type)? where? NL (promise | examples)* statement* "end" NL
params      = name ":" type ("," name ":" type)*
where       = "where" Name ":" Name ("," Name ":" Name)*
type        = Name ("." Name)? ("(" type ("," type)* ")")?
            | "(" type ("," type)+ ")"
            | "fn" "(" (type ("," type)*)? ")" (":" type)?
struct      = "struct" Name ("(" Name ("," Name)* ")")? NL (name ":" type NL)+ "end" NL
enum        = "enum" Name ("(" Name ("," Name)* ")")? NL (case NL)+ "end" NL
case        = Name | Name "(" type ")" | Name "(" name ":" type ("," name ":" type)* ")"
promise     = "promise" text NL (expr NL)+ (("allows" | "forbids") anchor ("," anchor)* NL)+ "end" NL
anchor      = name ":" expr
examples    = "examples" NL (expr NL)+ "end" NL
test        = "test" text NL statement* "end" NL
statement   = simple NL
            | "for" binding "in" expr NL statement* "end" NL
            | "while" expr NL statement* "end" NL
simple      = expr | block_if
            | target "=" (expr | block_if) | target ("+=" | "-=") expr | "var" name "=" (expr | block_if)
            | "return" expr? ("if" expr)? | ("break" | "continue") ("if" expr)? | "assert" expr
block_if    = "if" expr NL statement* ("else" "if" expr NL statement*)* ("else" NL statement*)? "end"
expr        = "if" expr ":" expr "else" (":" expr | expr)        # one-line if; "else if" chains
            | expr ("implies" | "or" | "and") expr | "not" expr
            | expr ("==" | "!=" | "<" | "<=" | ">" | ">=") expr | expr "is" pattern
            | expr ".." expr | expr ("+" | "-" | "*" | "/" | "%") expr | ("-" | "try") expr
            | expr "." (name | Name | digits) | expr "(" (arg ("," arg)*)? ")"
            | literal | name | Name | "(" expr ")" | "(" expr ("," expr)+ ")"
            | "[" (expr ("," expr)*)? "]" | "{" (expr ":" expr ("," expr ":" expr)*)? "}"
            | "case" expr NL arm (arm | statement)* "end"
            | "fn" "(" (name ("," name)*)? ")" (expr | NL statement*) "end"
            | "stop" text
arg         = expr | name ":" expr
arm         = pattern ("|" pattern)* ("if" expr)? ":" simple? NL
target      = name ("." name)* | "(" binding ("," binding)+ ")"
binding     = name | "_" | "(" binding ("," binding)+ ")"
pattern     = "_" | name | literal | "-" number | Name ("." Name)?
            | Name ("." Name)? "(" (pattern | name ":" pattern ("," name ":" pattern)*) ")"
            | "(" pattern ("," pattern)+ ")"
            | "[" (pattern ("," pattern)*)? "]" | "[" (pattern ",")* ".." (name | "_") "]"
```

A function's parameters, a call's arguments, a case's named fields, and
tuples, lists and maps may end with a comma.

Operators, from loosest to tightest. Comparisons, `..` and `implies` don't
chain; the others do, left to right.

| Level | Operators |
| --- | --- |
| 1 | one-line `if ... else ...` |
| 2 | `implies` |
| 3 | `or` |
| 4 | `and` |
| 5 | `not` |
| 6 | `==` `!=` `<` `<=` `>` `>=` `is` |
| 7 | `..` |
| 8 | `+` `-` |
| 9 | `*` `/` `%` |
| 10 | `-` (negation) and `try` |
| 11 | `.name`, `.0`, and calls |

## Appendix C. Habits from other languages

| Elsewhere | In Mo | Section |
| --- | --- | --- |
| `items[i]`, `map[key]` | `items.get(i)`, `map.get(key)`, each an `Option` | 3.7, 3.8 |
| `len(x)`, `x.length` | `list.size`; `text.byte_count` or `text.char_count` | 3.4, 14 |
| `null`, `nil`, `None` checks | `Option`: `x or default`, `case`, `is` | 8.1 |
| `throw`, `raise`, `try ... catch` | return `Error(...)`; pass it on with `try`; handle it with `case` | 8.2, 8.3 |
| `unwrap()`, `!` | `case`, with `stop "why it can't happen"` | 8.4 |
| `!x`, `&&`, `\|\|` | `not x`, `and`, `or` | 3.1 |
| `elif`, `elsif` | `else if` | 7.1 |
| `cond ? a : b` | `if cond: a else: b` | 7.1 |
| `match`, `switch`, `=>` | `case`, with arms written `Pattern: ...` | 7.2 |
| `x => ...`, `\|x\| ...`, `lambda x: ...` | `fn(x) ... end` | 5.6 |
| a closure that reads a variable assigned later | copy the value into a name first, `current = count`, or pass it as a parameter | 5.6 |
| `let x`, `const x`, `x := ...` | `x = ...`, or `var x = ...` to assign it again | 6 |
| `list.push(x)`, changing a list in place | `items = items.append(x)`, with `var items` | 3.7, 6.2 |
| `x = x.trim()` on a fixed name | `trimmed = x.trim` | 6.1 |
| `print(x)`, `console.log(x)` | `console.print("#{x}")`, with a `Console` from `main` | 11, 14 |
| `"a" + b` | `"a#{b}"` | 3.4 |
| `x.f()` with no arguments | `x.f` | 5.4 |
| `str(x)`, `x.toString()` | `x.to_text`, or `"#{x}"` inside text | 9.2 |
| `def`, `function`, `func` | `fn name(x: T) : R ... end` | 5.1 |
| `class`, methods, `self` | `struct` or `enum`, with functions in its module | 4, 5.4 |
| `import x`, `export` | `use A.B{x}`, and the `expose` line | 10 |
| `List<T>`, `Vec<T>` | `List(T)` | 9.1 |
| `->` before a result type | `:` | 5.1 |
| `;` | a line break | 2.4 |

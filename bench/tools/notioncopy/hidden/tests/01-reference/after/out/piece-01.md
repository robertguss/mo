<table_of_contents/>
**Version 0.1, approved on 12 October 2026.** This is core Mo, the language that later versions build services, live upgrades and approved intent on. It is written to be read in full, by people and by agents, before writing Mo.
Every rule has an example, and every code example parses with Mo's executable grammar (`grammar/` in this repository). "An error" below means the compiler rejects the program. "Stops" means a running program has hit a bug and ends with a report (section 13.2).
## 1. A first program
```ruby
## Prints each word given on the command line, in capitals.
module Shout

fn main(args: Args, console: Console)
  for word in args.list
    console.print(word.ascii_uppercase)
  end
end
```
- A file is one module, named on its first line of code. `##` comments describing it may come before.
- `main` lists the capabilities the program needs: here its arguments and the console. They are its whole authority. It can't touch files, the network or the clock, because it wasn't handed them (section 11).
- Blocks open with a keyword and close with `end`. Indentation carries no meaning; `mo format` lays code out with two spaces per level, in lines of at most 100 columns.
## 2. Source text
### 2.1 Files
A program is a tree of modules under one root folder. The module `Shop.OrderBook` lives in `shop/order-book.mo`: each part of its name becomes a lowercase folder or file name, with hyphens between words. A word starts at a capital letter that follows a lowercase letter or a digit, and at the last capital of a run that a lowercase letter follows, so `XMLReader` lives in `xml-reader.mo`, `HTTP2Server` in `http2-server.mo` and `Unicode16Tool` in `unicode16-tool.mo`.
Source is UTF-8. Characters that change how text looks without being seen are an error anywhere, even in text and comments: direction overrides, zero-width characters, and control characters other than tab and line breaks. In text, write them as escapes, such as `"\u{200B}"`.
### 2.2 Comments
`#` starts a comment that runs to the end of the line. `##` documents the declaration below it, or the module when it comes before `module`. `mo format` lines up the comments at the ends of consecutive lines in one column, two spaces after the longest line's code.
```ruby
## The price of an order, with tax.
fn total(order: Order) : Decimal
  order.subtotal + order.tax  # the tax is already rounded
end
```
### 2.3 Names
- Values, functions, fields and parameters are snake_case: lowercase ASCII letters, digits and `_`, starting with a letter, or with `_` for a name that is unused on purpose.
- Types, the cases of choice types, and modules are CapCase: `OrderBook`, `NotFound`.
- A type parameter is a single capital letter: `T`, `K`, `V`.
- A name ending in `?` is a yes-or-no answer: a `Bool`, or a function that returns one, such as `empty?` or `paid?`.
- `_` alone means "ignored", and a name starting with `_`, such as `_reason`, is unused on purpose.
Names are ASCII; text and comments may use any Unicode. These words are reserved:
```plain text
allows and assert break case continue else end enum examples expose false fn
for forbids if implies in is module not or promise return stop struct test true
try use var where while
```
### 2.4 Lines
A line break ends a statement; there are no semicolons. A statement continues on the next line when its line ends with an opening bracket, a comma, `.`, `=` or a binary operator, or when the next line starts with a closing bracket, `.`, `where` or a binary operator other than `-`. A statement also runs on through any block it opens, such as a `case` or an anonymous function written on several lines, to that block's `end`.
```ruby
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
These show where a statement may break. `mo format` chooses the breaks itself: a statement that fits in 100 columns goes on one line, so it would join each of these, and a longer one is broken at its outermost brackets, dots or operators first. It counts columns as `Unicode16.display_width` does (section 14), turns any run of blank lines between statements into one, and writes no trailing commas.
## 3. Values and types
<table header-row="true">
<tr>
<td>Type</td>
<td>Values</td>
<td>Section</td>
</tr>
<tr>
<td>`Bool`</td>
<td>`true` and `false`</td>
<td>3.1</td>
</tr>
<tr>
<td>`Int`, `BigInt`, `Byte`</td>
<td>whole numbers</td>
<td>3.2</td>
</tr>
<tr>
<td>`Decimal`, `Float`</td>
<td>fractions</td>
<td>3.3</td>
</tr>
<tr>
<td>`Text`, `Char`, `Bytes`</td>
<td>Unicode text, one character, binary data</td>
<td>3.4</td>
</tr>
<tr>
<td>`Unit`</td>
<td>just `Unit`, what a function that returns nothing returns</td>
<td>3.5</td>
</tr>
<tr>
<td>`(A, B)`</td>
<td>tuples</td>
<td>3.6</td>
</tr>
<tr>
<td>`List(T)`, `Map(K, V)`, `Set(T)`</td>
<td>collections</td>
<td>3.7, 3.8</td>
</tr>
<tr>
<td>`Range`</td>
<td>whole numbers from `a` up to `b`, written `a..b`</td>
<td>7.5</td>
</tr>
<tr>
<td>`Time`, `Duration`</td>
<td>instants and lengths of time</td>
<td>3.9</td>
</tr>
<tr>
<td>records and choice types</td>
<td>declared with `struct` and `enum`</td>
<td>4</td>
</tr>
<tr>
<td>`fn(A) : B`</td>
<td>functions</td>
<td>5.6</td>
</tr>
<tr>
<td>`Option(T)`, `Result(T, E)`</td>
<td>maybe-missing values, and success or failure</td>
<td>8</td>
</tr>
<tr>
<td>capabilities</td>
<td>`Console`, `Files` and the others</td>
<td>11</td>
</tr>
</table>
All values are immutable. A `var` (section 6.2) changes which value a name holds, never a value someone else holds. Mo never converts between types silently: a conversion is a function call, and one that could fail returns an `Option`. Conversions that round, such as `to_float` and `to_fixed`, say how they round. Comparing or combining values of different types is an error.
### 3.1 Bool
`and` and `or` evaluate their right side only when they need it, and `not` negates. A condition must be a `Bool`: no numbers, texts or lists stand in for one.
```ruby
ready = in_stock and not on_hold
if order.lines.empty?
  console.print("nothing to ship")
end
```
`a implies b` means `not a or b`. It reads best in promises (section 12.2).
### 3.2 Whole numbers
`Int` is a 64-bit signed whole number, and the type of counts, sizes and positions. Literals may put `_` between digits, and may be hexadecimal or binary:
```ruby
population = 8_100_000_000
mask = 0xFF
flags = 0b1010
```
`+`, `-`, `*`, `/` and `%` take two numbers of the same type. `/` rounds toward zero, and `%` gives the remainder with the sign of the left side: `-7 / 2` is `-3`, and `-7 % 2` is `-1`. `a.floor_div(b)` and `a.floor_mod(b)` round toward negative infinity instead.
Overflow, and dividing by zero, are bugs: the program stops. Code that expects a result might not fit says what it wants instead:
```ruby
fits = a.checked_add(b)            # an Option(Int): None if it doesn't fit
hash = h.wrapping_mul(31)          # wraps around
capped = level.saturating_add(10)  # caps at the largest Int
```
`BigInt` holds whole numbers of any size, with the same arithmetic: `n.to_big` converts to one, and `big.to_int` returns `None` when the value doesn't fit. `Byte` holds 0 to 255, for binary data; `n.to_byte` returns an `Option(Byte)`. `Byte` has no arithmetic (section 9.2): convert it with `to_int` first.
### 3.3 Decimal and Float
A literal with a decimal point, such as `19.99`, is a `Decimal`: an exact fraction with 18 decimal places, up to about ±1.7 × 10²⁰. Decimal arithmetic is exact while results fit in 18 places, so `0.1 + 0.2 == 0.3`. A result that needs more places, such as `1.00 / 3`, is rounded to the nearest 18th place, with an exact tie rounded to the even digit. Overflow and dividing by zero stop the program, as with `Int`, and `%` gives the remainder with the sign of the left side, as it does for `Int`. A decimal's text form is its shortest exact spelling: `15.5`, `3`, `0.001`.
`Float` is a 64-bit IEEE 754 binary floating-point number, for science and graphics. A number literal takes the type its context needs: in `x * 0.5`, with `x` a `Float`, `0.5` is a `Float`, and in `price * 2`, with `price` a `Decimal`, `2` is a `Decimal`. A literal with nothing to decide its type is an `Int`, or a `Decimal` if it has a decimal point. Dividing an `Int` by an `Int` gives an `Int`, so convert first to get a fraction: `total.to_decimal / count.to_decimal`. Converting is explicit:
```ruby
share = cents.to_decimal / 100  # Int to Decimal
ratio = share.to_float          # Decimal to Float, rounded to the nearest float
back = ratio.to_decimal         # an Option: a float may be infinite or not a number
rounded = share.round(2)        # 2 places, ties away from zero, as people round by hand
label = rounded.to_fixed(2)     # the text "0.10": exactly 2 places, rounded as round does
```
### 3.4 Text, Char and Bytes
`Text` is Unicode text. It is stored as UTF-8 and is always valid. Text is written in double quotes, with the escapes `\n`, `\t`, `\r`, `\\`, `\"`, `\'`, `\#` and `\u{1F600}` (a character by its hexadecimal code). `#{...}` inside text inserts the text form of any value that has one (section 9.2), and may hold any expression, even one with text in it, as in `"#{name or "you"}"`:
```ruby
greeting = "Hello, #{name}! You have #{count} new messages."
literal = "write \#{name} to insert a name"
```
Text on several lines starts with `"""` at the end of a line and ends with `"""` first on a line, after its indentation; code may go on after it on that line, as in `""")`. It holds the lines between them, without the line break before the closing `"""`, which sets the indentation every line drops:
```ruby
usage = """
  Usage: report [--json] FILE
  Prints a summary of FILE.
  """
```
A `Char` is one Unicode character (a code point), in single quotes: `'a'`, `'\n'`, `'é'`.
Text has no single "length". `text.byte_count` counts UTF-8 bytes, instantly; `text.char_count` counts characters, reading the text. There is no `text[i]`: use whole-text operations such as `split`, `lines`, `trim`, `starts_with?` and `replace`, or walk `text.chars`. Two texts are equal when their bytes are equal, and texts sort by character code. Mo never normalizes text silently. The `Unicode16` module (section 14) normalizes text, splits it into user-perceived characters, and converts case beyond ASCII.
`Bytes` is binary data. `text.bytes` always succeeds; `bytes.decode_utf8` returns `None` when the bytes aren't valid UTF-8.
### 3.5 Unit
`Unit` is the type with one value, also written `Unit`. A function with no result type returns it. `Result(Unit, E)` is the result of an action that can fail but has nothing to return.
### 3.6 Tuples
A tuple groups two or more values: `(3, "three")` has type `(Int, Text)`. `pair.0` and `pair.1` read its parts, and a pattern takes it apart:
```ruby
(low, high) = bounds(readings)
spread = high - low
```
### 3.7 Lists
`List(T)` is Mo's sequence: `[1, 2, 3]`, or `[]` when empty. Every element has the same type. Reading by position returns an `Option`, because the position may be past the end:
```ruby
first = items.first       # an Option
third = items.get(2)      # positions start at 0
longer = items.append(4)  # a new list; items itself is unchanged
```
Functions that "change" a list return a new one, so `items.append(4)` on a line of its own is an error: the new list is unused. With a `var`, write `items = items.append(4)`. When the old list has no other holder, the new one reuses its memory, so building a list in a loop costs what it would with a mutable array (section 13.3).

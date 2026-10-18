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

### 6.2 Variables
`var name = value` declares a name that can be assigned again within the function that declares it. Assigning to a field, or to a field of a field, builds a new record with that field changed:
```ruby
var cart = Cart(owner: "ana", items: [], total: 0)
cart.items = cart.items.append(item)
cart.total += item.price
```
`+=` and `-=` add to and subtract from a `var` or its fields; `mo format` writes `x = x + y` as `x += y`. Assignment never changes a value anyone else holds: whoever holds the old cart still sees the old cart. When the `var` is its value's only holder, the new value reuses its memory.
No parameter is passed by reference: a function returns a new value, and its caller assigns it.
## 7. Control flow and patterns
### 7.1 if
`if` runs a block when its condition is true, with optional `else if` and `else` blocks. With an `else`, an `if` has a value: the value of the block that ran. A block `if` stands on a line of its own or after `=`.
```ruby
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
A value that fits on one line uses the one-line form, which always has an `else` and may chain:
```ruby
unit = if count == 1: "item" else: "items"
sign = if n < 0: "negative" else if n == 0: "zero" else: "positive"
```
### 7.2 case
`case` compares a value with patterns, top to bottom, and runs the first arm that matches. An arm is `Pattern: statement` on one line, or `Pattern:` followed by lines that run until the next arm or `end`. The value of a `case` is the value of the arm that ran.
```ruby
message = case outcome
  Ok(order): "placed order #{order.id}"
  Error(UnknownItem(name: n)): "no such item: #{n}"
  Error(BadQuantity(problem)):
    detail = describe(problem)
    "bad quantity: #{detail}"
end
```
The arms must cover every possible value; if they don't, the error lists what's missing. When the value is a choice type, a whole arm can't be a catch-all, `_` or a name: list the cases, so that a case added later shows every place that must handle it. Inside a pattern, `_` and names may stand for anything, as in `Error(_)`. Arms with the same body share a line, as in `Saturday | Sunday: "weekend"`, and an arm may have a guard, as in `Some(n) if n > 10: "many"`.
### 7.3 Patterns
<table header-row="true">
<tr>
<td>Pattern</td>
<td>Matches</td>
</tr>
<tr>
<td>`_`</td>
<td>anything, and ignores it</td>
</tr>
<tr>
<td>`name`</td>
<td>anything, and binds it to `name`</td>
</tr>
<tr>
<td>`42`, `-1`, `2.5`, `"GET"`, `'x'`, `true`</td>
<td>that literal value</td>
</tr>
<tr>
<td>`None`, `Some(p)`, `Circle(p)`</td>
<td>that case, with its value matching `p`</td>
</tr>
<tr>
<td>`Rectangle(width: p, height: q)`</td>
<td>that case or record, with the named fields matching; fields not named are ignored</td>
</tr>
<tr>
<td>`(p, q)`</td>
<td>a tuple</td>
</tr>
<tr>
<td>`[]`, `[p, q]`, `[first, ..rest]`, `[first, .._]`</td>
<td>an empty list, a list of exactly two, and a list of at least one, with or without a name for the rest</td>
</tr>
</table>
A case with named fields is matched by naming them, even when it has only one: `NotFound(path: p)`, or `NotFound(path: _)` to ignore the path. The form `Circle(p)` is only for a case's one unnamed value.
### 7.4 is
`value is Pattern` tests a pattern, giving a `Bool`. The names the pattern binds are visible where the test is known to be true: in the block of the `if` or `while` it guards, after `and` in the same condition, after `implies`, and after an `assert`.
```ruby
if reply is Ok(body) and not body.empty?
  console.print(body)
end
while console.read_line is Some(line)
  console.print(line.ascii_uppercase)
end
```

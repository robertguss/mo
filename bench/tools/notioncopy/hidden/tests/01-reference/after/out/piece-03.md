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
### 7.5 Loops
`for` runs a block for each element of a list, set, map or range, and `while` runs a block while a condition is true. `break` leaves the loop, and `continue` starts its next round. A range `a..b` counts from `a` up to, but not including, `b`.
```ruby
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
Mo has no null and no exceptions. A value that may be missing is an `Option`; a function that can fail returns a `Result`; and a bug stops the program (section 13.2). Both are ordinary choice types:
```ruby
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
`x or default` gives the value inside a `Some`, or the default for `None`, and computes `default` only for `None`. `x.or_fail(e)` turns `None` into `Error(e)`; it is a function, so `e` is computed even when `x` is a `Some`. `case` and `is` handle both cases:
```ruby
city = address.city or "unknown"
case users.get(id)
  Some(user): console.print(user.name)
  None: console.print("no user #{id}")
end
```
### 8.2 Results and declared failures
A function that can fail declares its failures as a choice type, and returns a `Result`:
```ruby
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
`try expr` takes the value out of an `Ok`, or returns the `Error` from the function it is in. It works on a `Result` in a function that returns a `Result`, and on an `Option` in a function that returns an `Option`. In a function that returns neither, such as `main : Int` or a test, handle the failure with `case`. When the failure types differ, `try` wraps the failure if the function's failure type has exactly one case that holds it as its unnamed value, as `BadQuantity(QuantityError)` does:
```ruby
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
With no such case, or more than one, `try` is an error that says which case to add. Ignoring a `Result` is an error too: every failure is passed upward or handled.
### 8.4 Can't happen
There is no "unwrap". Where a failure truly can't happen, the code says why, with `stop`:
```ruby
rate = case rates.get(currency)
  Some(r): r
  None: stop "every currency was checked against the rate table at startup"
end
```
Reaching a `stop` is a bug: the program stops and reports the reason. A `stop` fits wherever a value is expected, because control never comes back from it. So does a block or arm that ends in `return`: it leaves the function instead of giving a value.
## 9. Generics and abilities
### 9.1 Generic types
`List(T)`, `Map(K, V)` and your own types take type parameters:
```ruby
struct Page(T)
  items: List(T)
  number: Int
end
```
A generic type can't hold itself at a bigger type built from its parameter, as a `Tree(T)` with a field `children: List(Tree(List(T)))` would. Each type a generic type is used with becomes a type of its own, and that one would need types without end, as such a call would for a generic function (section 5.5). `children: List(Tree(T))` is fine.
### 9.2 The five abilities
An ability is something a type can do. There are five, all built in:
<table header-row="true">
<tr>
<td>Ability</td>
<td>What it gives</td>
<td>Customized with</td>
</tr>
<tr>
<td>`Equal`</td>
<td>`==` and `!=`</td>
<td>nothing: values are equal when their fields are</td>
</tr>
<tr>
<td>`Hash`</td>
<td>use as a map key or a set member</td>
<td>nothing: always structural</td>
</tr>
<tr>
<td>`Order`</td>
<td>`<`, `<=`, `>`, `>=`, `sort`, `min` and `max`</td>
<td>`fn compare(a: T, b: T) : Ordering`</td>
</tr>
<tr>
<td>`Text`</td>
<td>a text form, for `#{...}` and printing</td>
<td>`fn to_text(x: T) : Text`</td>
</tr>
<tr>
<td>`Number`</td>
<td>`+`, `-`, `*`, `/`, `%` and `sum`</td>
<td>nothing: only `Int`, `BigInt`, `Decimal` and `Float` have it; times and durations also add and subtract (3.9)</td>
</tr>
</table>
```ruby
enum Ordering
  Less
  Equal
  Greater
end
```
A type with `Hash` or `Order` also has `Equal`. Ability names appear only after `where`, so they don't clash with other names. Records and choice types get `Equal`, `Hash`, `Order` and `Text` automatically whenever all their fields have them. Equality compares field by field; maps compare their entries, and sets their elements, in any order. Order compares fields in the order they are declared, and cases in the order they are declared. Functions and capabilities have no abilities. The built-in types have these:
<table header-row="true">
<tr>
<td>Type</td>
<td>`Equal`</td>
<td>`Hash`</td>
<td>`Order`</td>
<td>`Text`</td>
<td>`Number`</td>
</tr>
<tr>
<td>`Bool`</td>
<td>yes</td>
<td>yes</td>
<td>`false` first</td>
<td>yes</td>
<td>no</td>
</tr>
<tr>
<td>`Int`, `BigInt`, `Decimal`</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
</tr>
<tr>
<td>`Byte`</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>no</td>
</tr>
<tr>
<td>`Float`</td>
<td>IEEE: "not a number" equals nothing</td>
<td>no</td>
<td>yes, with "not a number" last</td>
<td>yes</td>
<td>yes</td>
</tr>
<tr>
<td>`Text`, `Char`</td>
<td>yes</td>
<td>yes</td>
<td>by character code</td>
<td>yes</td>
<td>no</td>
</tr>
<tr>
<td>`Bytes`</td>
<td>yes</td>
<td>yes</td>
<td>byte by byte</td>
<td>no</td>
<td>no</td>
</tr>
<tr>
<td>`Unit`</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>no</td>
</tr>
<tr>
<td>`Time`, `Duration`</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>yes</td>
<td>no (but they add and subtract, 3.9)</td>
</tr>
<tr>
<td>`Range`</td>
<td>yes</td>
<td>yes</td>
<td>no</td>
<td>yes: `0..3`</td>
<td>no</td>
</tr>
<tr>
<td>tuples</td>
<td>when every part has it</td>
<td>same</td>
<td>same, part by part from the left</td>
<td>same</td>
<td>no</td>
</tr>
<tr>
<td>`List(T)`</td>
<td>when `T` has it</td>
<td>same</td>
<td>same, element by element; a list comes before any longer list that begins with it: `[1, 2] < [1, 2, 0]`</td>
<td>same</td>
<td>no</td>
</tr>
<tr>
<td>`Map(K, V)`, `Set(T)`</td>
<td>when their keys and values have it, comparing entries in any order</td>
<td>same</td>
<td>no</td>
<td>same</td>
<td>no</td>
</tr>
</table>
`Option`, `Result` and `Ordering` are choice types, so their cases are ordered as they are declared: `None`, `Some`; `Ok`, `Error`; `Less`, `Equal`, `Greater`. `Float` follows IEEE rules: "not a number" equals nothing and sorts last, and floats have no `Hash`, so they can't be map keys.
A value's text form looks like the code that builds it: `Point(x: 1.5, y: 2)`, `Some(3)`, `[1, 2]`, `{"a": 1}`, `(1, true)`, `[1, 2].to_set`. A `Text` or `Char` value's text form is itself; inside another value's, it is quoted and escaped as in code. Numbers have their shortest spelling. A `Float` is written in plain digits, as `1000` or `0.25`, unless its size is below 1e-6 or at least 1e21, when it has an exponent, as `1e21` or `1.5e-7`. A whole float has no `.0`, zero is `0` and negative zero `-0`, and a float that is not a number is `nan`, the infinities `inf` and `-inf`. A `Bool` is `true` or `false`, a `Time` is RFC 3339 text, and a `Duration` is in seconds, as `1.5s`. `x.to_text` gives the text form of any value whose type has `Text`: the same text as `"#{x}"`.
`x.compare(y)` compares two values whose type has `Order`, giving `Less`, `Equal` or `Greater`.
A type customizes `Order` or `Text` with one ordinary function in its own module, which applies wherever the type goes, exposed or not:
```ruby
struct Money
  cents: Int
  currency: Text
end

fn to_text(money: Money) : Text
  "#{(money.cents.to_decimal / 100).to_fixed(2)} #{money.currency}"
end
```
There are no user-defined abilities in v0.1. Code that needs something other than these five takes a function as a parameter.
## 10. Modules
### 10.1 The header
A module starts with its name, then the `expose` line listing its public names, then its `use` lines:
```ruby
## Orders: building, pricing and checking them.
module Shop.Orders
expose Order, OrderError(..), place, total
use Shop.Catalog
use Shop.Money{Money, add}
```
Everything not on the `expose` line is private to the module. Tests in the same file see everything (section 12.3).
### 10.2 Hidden insides
`expose Order` exposes the type without its fields or cases. Outside the module an `Order` can't be built, taken apart or have its fields read; it can only be passed to the module's functions. `expose OrderError(..)` exposes the fields or cases too. Hidden insides keep values valid: if `place` is the only way to get an `Order`, every `Order` has passed `place`'s checks.
### 10.3 use
`use Shop.Catalog` lets this module write `Catalog.find(...)` and `Catalog.Item`. `use Shop.Money{Money, add}` also brings those names in unqualified, and a choice type's exposed cases come with it. A module may declare a type with its own name, as the module `Account` declares `Account`: `Account.open(...)` calls the module's function, and `Account` in a type is the type. There are no wildcards and no aliases. A clash between names is an error that suggests the qualified form, and so is a cycle of modules that use each other. The library's modules, `Json` and `Unicode16`, are used the same way, and a program can't declare a module with one of their names.

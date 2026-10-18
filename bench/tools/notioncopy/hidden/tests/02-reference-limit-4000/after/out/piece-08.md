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

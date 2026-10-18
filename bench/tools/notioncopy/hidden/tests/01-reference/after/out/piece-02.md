### 3.8 Maps and sets
`Map(K, V)` maps keys to values: `{"apples": 3, "pears": 5}`, or `{}` when empty. Keys are expressions, so `{fruit: 3}` uses the value of `fruit`. A literal that gives the same key twice is an error when the compiler can see it, as with two equal literals or the same name twice, and a bug when two keys turn out equal as the program runs. `Set(T)` holds distinct values: `Set.empty`, a constant, or `items.to_set`. Keys and set members need equality and hashing, which most types have (section 9.2).
Maps and sets keep insertion order, also after removals: iterating visits keys in the order they were added, setting an existing key keeps its place, and a key removed and added again goes last. Two maps are equal when they hold the same entries, in any order. Iterating a map gives `(key, value)` tuples.
```ruby
stock = {"apples": 3}
more = stock.set("pears", 5)
count = more.get("apples") or 0  # get returns an Option
for (fruit, n) in more
  console.print("#{fruit}: #{n}")
end
```
### 3.9 Time and Duration
A `Time` is an instant between the years 1 and 9999, read from a `Clock` (section 11). A `Duration` is a length of time, made from an `Int`: `5.seconds`, `200.milliseconds`, `3.minutes`, `2.hours`, `7.days`. Both count whole microseconds. Subtracting two times gives a `Duration`, and never overflows; adding a `Duration` to a `Time`, or subtracting one from it, gives a `Time`; durations add, subtract and multiply by an `Int`. A time outside the years 1 to 9999 is a bug, so `time.checked_add(duration)` returns an `Option`, `None` outside them, for code that can't rule it out; a negative duration goes back in time. Times and durations compare with `<` and the other comparisons.
```ruby
expires = now + 30.minutes
an_hour_ago = now - 1.hours
late? = now - order.placed > 2.days
```
## 4. Records and choice types
### 4.1 Records
A `struct` declares a record type: named fields, each with a type. A record is built by naming every field, in any order, and its fields are read with `.`:
```ruby
struct Point
  x: Decimal
  y: Decimal
end

origin = Point(x: 0, y: 0)
sum = origin.x + origin.y
```
When a record is built, its fields are computed in the order they are written there, whatever their order in the declaration. There are no optional fields; a field that may be missing has an `Option` type. Records have no methods: functions in the record's module work on it (section 5.4).
### 4.2 Choice types
An `enum` declares a choice type: each value is exactly one of its cases. A case carries no data, one unnamed value, or named fields:
```ruby
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
A case is built like a record, by naming its fields, or from its one unnamed value: `Circle(2.5)`. Types may be generic and recursive:
```ruby
enum Tree(T)
  Leaf
  Node(left: Tree(T), value: T, right: Tree(T))
end
```
A module's cases share one name space: no two of its enums have a case of the same name, and no case reuses the name of a type or case that is always in scope, such as `Set`, `None` or `Error`. `Option` and `Result` are ordinary choice types (section 8).
## 5. Functions
### 5.1 Definitions
```ruby
fn shipping(weight: Decimal, express?: Bool) : Decimal
  base = 4.50 + weight * 0.75
  if express?: base * 2 else: base
end
```
A signature gives every parameter's type and the result's type; a function with no result type returns `Unit`. Inside a body, types are inferred from all their uses in it. A body is a sequence of statements, and its value is the value of its last line. `return` leaves early. A trailing `if`, a guard, is allowed only on the three early exits: `return`, `break` and `continue`.
```ruby
fn discount(total: Decimal, member?: Bool) : Decimal
  return 0 if not member?
  return total * 0.10 if total > 100
  total * 0.05
end
```
Functions are declared at the top level of a module. There are no default or optional parameters. Two functions in a module share a name only when their first parameters are different types declared in that module, so both `Order` and `OrderLine` can have a `total`; a call picks the one whose first parameter matches.
### 5.2 Calls
Arguments are passed by position: `shipping(2.5, false)`. Only records and cases are built with named fields. A function with no parameters is called with empty parentheses: `defaults()`.
### 5.3 Recursion
Functions may call themselves and each other. A call in tail position, the last thing a function does, takes the caller's place on the stack instead of adding to it, so a recursive loop can run as long as it needs to:
```ruby
fn sum_to(n: Int, total: Int) : Int
  if n == 0: total else: sum_to(n - 1, total + n)
end
```
### 5.4 Dot calls
`x.f(a, b)` calls `f(x, a, b)`, and `x.f`, with no other arguments, calls `f(x)`: write `x.f`, not `x.f()`. Mo looks for `f` first in the module that declares the type of `x`, then among the names in scope. Chains read left to right: `text.trim.lines.size`.
```ruby
words = sentence.split(" ")  # Text's split
loud = shout(words)          # a function in this module
same = words.shout           # the same call
```
Where a record's field and a function share a name, `x.name` is the field where the field is visible, and the function elsewhere (section 10.2). When the field holds a function, `x.name(a)` calls it with `a`.
### 5.5 Generic functions
A single capital letter in a signature is a type parameter, and `where` asks for abilities (section 9):
```ruby
fn largest(items: List(T)) : Option(T) where T: Order
  items.sort.last
end
```
A generic function can't call itself, directly or through other functions, with a type parameter standing for a bigger type built from that parameter. If `depth(t: Tree(T))` leads to a call of `depth` on a `Tree(List(T))`, `T` stands for `List(T)` there, and that is an error. Calling `depth` on a node's `left`, another `Tree(T)`, is fine. Each type a generic function is used with gets its own copy of the function, and such a call would need copies without end: for `Tree(List(T))`, `Tree(List(List(T)))`, and so on.
### 5.6 Functions as values
A named function is a value: `prices.map(with_tax)`. An anonymous function is written `fn(x) ... end`, on one line or several, and its parameter types are inferred. A function type is written like a signature: `fn(Decimal) : Decimal`, or `fn(Text)` for one that returns `Unit`.
```ruby
taxed = prices.map(fn(p) p * 1.2 end)
labels = orders.map(fn(order)
  status = if order.paid?: "paid" else: "due"
  "#{order.id}: #{status}"
end)
```
An anonymous function can read names in scope where it is made, but it can't assign a `var` and can't capture a capability (section 11.4), so it can do only what its parameters allow; it may take a capability as a parameter. So printing each item works in a `for` loop, not inside `map`.
An anonymous function may read a `var` from the function around it only if nothing assigns that `var` after the anonymous function is made, including in a later round of a loop around it. Reading any other `var` is an error, since it would be unclear whether the function sees the value it was made with or a later one. Copy the value into a name first, and read the name:
```ruby
var count = 0
current = count
show = fn() "#{current}" end  # fn() "#{count}" end would be an error
count += 1
console.print(show())  # prints 0
```
To use a later value, make the function after the last assignment, or pass the value as a parameter.
`return` and `try` inside an anonymous function leave that anonymous function, not the function around it.
## 6. Names, bindings and variables
### 6.1 Bindings
`name = value` binds a name once. A name is visible from its binding to the end of its block: a function's or a test's body, a branch of an `if`, an arm of a `case`, a loop's body, or an anonymous function's body.
Within a function or a test a name means one thing. While a local name, a parameter or a bound name, is visible it can't be bound again, even in an inner block or an anonymous function's parameters. So `line = line.trim` is an error: write `trimmed = line.trim`, or use a `var`. Separate blocks, such as two arms of a `case`, may use the same name. A function also can't refer to one of its module's functions or constants by a name it uses for a local: `fn open(owner: Text)` is fine in a module with an `owner` function, as long as `open` doesn't call `owner(...)`.
Unused names, parameters and imports are errors, and so is a result that isn't `Unit` and isn't used. A name starting with `_` is unused on purpose, and `for _ in 0..3` repeats without a name.
```ruby
subtotal = order.lines.map(fn(line) line.price end).sum
tax = subtotal * 0.2
```
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

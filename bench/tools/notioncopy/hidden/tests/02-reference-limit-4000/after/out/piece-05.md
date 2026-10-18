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

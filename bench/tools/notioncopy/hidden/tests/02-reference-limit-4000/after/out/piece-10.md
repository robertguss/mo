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
### 10.4 Constants
A module may bind fixed values at its top level:
```ruby
max_attempts = 3
default_currency = "EUR"
```
A constant's type comes from its value, which must decide it; for anything else, such as a list of anonymous functions, write a function with a signature. Constants are computed once, before any other code of the program runs, in the order of their dependencies. A constant depends on the constants it uses, directly or through the functions it calls. So a constant may use one written below it, or one in another module, but constants that depend on each other in a cycle are an error. A constant that stops stops the program before it starts. There is no module-level state: values that last and change belong to processes, which come in a later version.

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

### 10.4 Constants
A module may bind fixed values at its top level:
```ruby
max_attempts = 3
default_currency = "EUR"
```
A constant's type comes from its value, which must decide it; for anything else, such as a list of anonymous functions, write a function with a signature. Constants are computed once, before any other code of the program runs, in the order of their dependencies. A constant depends on the constants it uses, directly or through the functions it calls. So a constant may use one written below it, or one in another module, but constants that depend on each other in a cycle are an error. A constant that stops stops the program before it starts. There is no module-level state: values that last and change belong to processes, which come in a later version.
### 10.5 Always in scope
Always in scope: the built-in types of section 3, `Option` with `Some` and `None`, `Result` with `Ok` and `Error`, `Ordering` with its cases, the capability types, `FileError` with its cases, and `Entry` with `EntryKind` and its cases. The functions of section 14 belong to their types' modules: call them with a dot, as in `text.lines`, or through the module, as in `Set.empty` and `Time.parse(text)`. So a local may be called `lines` or `count` without clashing with them.
## 11. Capabilities and effects
### 11.1 What a function can touch
A function can affect or observe the world outside the program only through capabilities it is handed as parameters. A capability's type names what it can do: `Console` prints, `Files` reads and writes files, `Clock` tells the time. So a function's parameter types are its complete list of effects, and a function with no parameter that is or holds a capability only computes: given the same arguments, it always gives the same result.
<table header-row="true">
<tr>
<td>Capability</td>
<td>Lets a program</td>
</tr>
<tr>
<td>`Args`</td>
<td>read its command-line arguments</td>
</tr>
<tr>
<td>`Env`</td>
<td>read environment variables</td>
</tr>
<tr>
<td>`Console`</td>
<td>print to standard output and standard error, and read standard input</td>
</tr>
<tr>
<td>`Files`</td>
<td>read and write files in one folder and the folders inside it</td>
</tr>
<tr>
<td>`ReadFiles`</td>
<td>only read them</td>
</tr>
<tr>
<td>`Clock`</td>
<td>read the current time</td>
</tr>
<tr>
<td>`Random`</td>
<td>draw random numbers</td>
</tr>
</table>
### 11.2 main
`main` lists the capabilities the program needs, in any order and at most one of each type. The runtime hands over exactly those, so a program's whole authority is on its first line. The `Files` or `ReadFiles` it gets reach the folder the program was started in.
`main` returns nothing, and the program exits with status 0; or an `Int`, the exit status, from 0 to 255; or `Result(Unit, E)`, where `Error(e)` prints `e`'s text form to standard error and exits with status 1.
```ruby
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
A capability can be narrowed before it is passed on. `files.within("reports")` reaches only that folder, and `files.read_only` returns a `ReadFiles`, which can only read. Calling `within` with a path that would leave the folder, such as `".."`, gives a capability that reaches nothing (section 14). Pass each function the least it needs:
```ruby
summary = try summarize(files.within("reports").read_only)
```
### 11.4 Rules
- Records may hold capabilities, so a function that needs several can take one record.
- Anonymous functions can't capture capabilities, or values holding them. A function that needs one takes it as a parameter.
- Only the runtime makes real capabilities. Tests, examples and promises use fakes (section 12.4).
- Capabilities have no abilities: they can't be compared, printed or used as keys.
- There are no log statements. To report something, print it or return it.
## 12. Examples, promises and tests
A function's specification sits at the top of its body: its promises and examples, then a blank line, then its code.
### 12.1 Examples
`examples` lists calls with their expected results. Each line is a `Bool` expression, usually `call == expected`, and `mo test` checks every line:
```ruby
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
- **a body**: one or more `Bool` lines over the parameters and `result`, the function's result, which must all be true;
- **anchors**: at least one `allows` line and one `forbids` line. Each gives every parameter and `result` a value, which the body must accept or reject.
```ruby
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
The compiler checks that the body accepts every `allows` and rejects every `forbids`, which catches a body that doesn't say what its sentence says. `mo test` checks the body against the function on many generated inputs, boundaries first. It builds a type exposed without its insides only through a limited number of calls to its module's exposed functions, other types from any values of their fields, and capabilities as fakes. A promise speaks about calls that return; a call that stops on a generated input is reported as a bug, with the input. The boundaries include each type's extremes, such as `Decimal`'s largest value and the last instant of the year 9999, so a promise that passes holds for every call its types allow. When an extreme stops a call, the report says so, as in "stops with overflow when `amount` is `Decimal`'s largest value", and shows the two fixes: return a failure for such inputs, or take a type whose module keeps its values in range. Proofs come in a later version.
A promise about many steps, such as "an accepted job is never lost", is a promise on the function that takes one step: something each step keeps true between its input and its result.
There are no preconditions. What a caller must guarantee becomes a type, such as a `Quantity` that its module never builds negative, or a failure, such as `Error(InvalidAmount)`.
### 12.3 Tests
A `test` names a scenario in a sentence and checks it with `assert`. Tests live in the same file as the code and see its private names. A name bound by `assert ... is Pattern` stays in scope for the rest of its block:
```ruby
test "a quantity is read past spaces"
  assert parse_quantity(" 12 ") is Ok(n)
  assert n == 12
  assert parse_quantity("-1") == Error(Negative(value: -1))
end
```
### 12.4 Fakes
Tests, examples and promises make fake capabilities, which reach nothing outside the program: `Console.fake()`, whose `printed` and `printed_errors` list the lines written to standard output and standard error; `Console.fake_input(lines)`, which also has those lines to read; `Files.in_memory(files)`, from a map of paths to texts; `Clock.fixed(time)`; `Random.seeded(n)`; `Args.of(list)`; and `Env.of(map)`. Fakes are made only there, never in a module's constants or its functions. `printed` and `printed_errors` can be used only there too, where every console is a fake, and using them anywhere else is an error.
A capability is a handle on something outside the program, and a fake is a handle on something kept for the test. Every use of one fake sees the same files and the same output: a file that `main` writes to a `Files.in_memory(...)` fake can be read back, and a `Console.fake()` passed to `main` shows afterwards what `main` printed.
```ruby
test "head prints the first line of each file"
  console = Console.fake()
  files = Files.in_memory({"a.txt": "one\ntwo\n"}).read_only
  assert main(Args.of(["a.txt"]), console, files) == Ok(Unit)
  assert console.printed == ["one"]
end
```
## 13. Running
### 13.1 Evaluation
Mo evaluates eagerly, left to right: a call's arguments, then the call; an operator's left side, then its right. `and`, `or` and `implies` skip their right side when the left decides the answer, and `x or default` on an `Option` computes `default` only when `x` is `None`. `try` returns early, and `stop` stops the program.
### 13.2 Bugs
A bug stops the program at once, with a report of what happened and where, and exit status 70. These are bugs: overflow in `Int`, `Decimal` or `Duration` arithmetic, or a time outside the years 1 to 9999; dividing by zero; reaching a `stop`; using more stack, call depth or memory than section 13.4's limits allow, or more memory than the machine has; calling a library function outside what it accepts, such as `take(-1)` or `random.int(5, 5)`; two keys of a map literal that turn out equal (section 3.8); an exit status outside 0 to 255; printing to an output that is gone; having too many folders open at once; and a fake whose files make one path both a file and a folder (section 14). Bugs are never caught.
### 13.3 Costs
Mo promises what operations cost, not how values are stored:
<table header-row="true">
<tr>
<td>Operation</td>
<td>Cost</td>
</tr>
<tr>
<td>`get`, `size`, `first` and `last` on a list</td>
<td>constant</td>
</tr>
<tr>
<td>`append` on a list, and assigning a `var`'s field</td>
<td>constant when the value has one holder; a copy when it's shared</td>
</tr>
<tr>
<td>`get` on a map, and `has?` on a map or a set</td>
<td>constant on average</td>
</tr>
<tr>
<td>`set` and `remove` on a map, and `add` and `remove` on a set</td>
<td>constant on average when the value has one holder; a copy when it's shared</td>
</tr>
<tr>
<td>`byte_count`</td>
<td>constant</td>
</tr>
<tr>
<td>`char_count`, `split`, `lines`, `contains?`</td>
<td>proportional to the text's length</td>
</tr>
<tr>
<td>`sort`, `sort_by` and `sort_by_desc`</td>
<td>n log n, and stable: equal keys keep their order</td>
</tr>
</table>
A value is shared when two names, fields or collections hold it. Changing a value with one holder reuses its memory; changing a shared value copies it first, and its other holders keep the old value.

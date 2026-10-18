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

# Write a finished Mo program

This page is the procedure for a finished Mo program. Correct Mo is Language Reference v0.1. `mo guide --reference` prints that reference. Section 13.4 says this guide covers how to work with the `mo` command, a small complete program with its tests, and when a task is done.

## Know this build

The language headings below use the present tense of the reference.

This build runs `mo check`, `mo run`, `mo test`, `mo format`, `mo fix`, `mo guide` and `mo emit`. The program below checks with no errors, runs, and passes its examples and its test.

`mo fix FILE` makes each edit that `mo check` shows as an error's only fix, checks again, and prints the errors that are left, as `mo check` does. It rewrites only the files it is given.

`mo emit FILE -o OUT` checks the program as `mo check` does, then writes its bytecode to `OUT`. `-o` is required, and `OUT` is never a source file of the program. A library module with no `main` is E0079, as for `mo run`, and a file that cannot be written is E1404. This build has no command that runs a bytecode file; run the source with `mo run`.

This build checks, formats and fixes `service ... end` blocks (reference 11.5). `mo test` still calls ordinary handler examples, promises and tests; it does not start the service or read its token. Only `mo run` on a service entry module refuses with E1408, "this build cannot run a service". Keep the service block and test its handlers directly until the runtime is implemented. Importing a module that declares a service does not start it.

A service tool that returns only its state gets E0101, "a service tool returns only its state". Choose between returning `(state, Unit)` to save new state and returning the state inside a record as read-only data. `mo fix` applies neither alternative. The suggested adapters keep the tool's name and `##` comment, move the original function to a helper and rebind the module's references to that helper. Exposed functions and functions whose names end in `?` receive no fixes. A `?` name must still return `Bool`. A selected generic handler gets E0100, "a service handler is generic". State, handler inputs and handler results must contain neither capabilities nor function values; E0104 reports this restriction.

A few forms the reference requires are still unfinished. `mo check` reports E1103 for a form the checker has not implemented, with a headline that says so, such as "this constant's call dependencies are not implemented". It reports E1150 for a form it checks but cannot compile yet, such as a constant's function value that reads a later constant. The headline is "this constant's deferred function needs globals not yet initialized". E1103 and E1150 mean this toolchain has not implemented that form. `mo run` and `mo test` report the same code for the same form.

If the reference offers another way to write the same behaviour from forms that compile, use that instead. Keep the unfinished form only when the reference requires it and offers no other way to write the same behaviour. For example, on a build where `List.sum` gives E1150, sum a `List(Int)` with `xs.reduce(0, fn(total, x) total + x end)`.

Every `Int`, `Float` and `Bytes` function listed in reference section 14 compiles and runs, whether called with a dot, through its type's module, or as a function value. This includes checked, wrapping and saturating integer arithmetic, integer conversions, float rounding and classification, and UTF-8 decoding.

`Byte.to_int` and `List.sum` also compile and run through dot calls, module calls and function values. `sum` supports `Int`, `BigInt`, `Decimal` and `Float`, adds from the left, and returns the type's zero for an empty list. Integer and decimal overflow stop the program, as with `+`.

Every `BigInt` function in reference section 14 compiles and runs, including `abs`, `min`, `max`, `pow`, `floor_div`, `floor_mod` and `to_int`. `Random.int` and the fake constructor `Random.seeded` also compile, including as function values.

`T.to_text(x)` and `T.compare(a, b)` work as well as `x.to_text` and `a.compare(b)` for every type with `Text` or `Order`, written through the type (`Text.compare`, `Option.to_text`), a type parameter, or the module that declares the type (`Shapes.to_text`), and `T.to_text` and `T.compare` are function values. A case without fields can be the receiver directly, as in `Less.compare(Greater)` or `Folder.to_text`. `Ordering`, `Entry`, and `Map` support `to_text` and interpolation. Maps print entries in insertion order, with text and character keys and values quoted and escaped.

`Env.get` runs on real or fake capabilities. Tests, examples and promises can construct a fake with `Env.of`.

`mo test` exits 1 with E1400 when it cannot build the inputs of a selected promise, such as one whose function takes a function. The headline is "this build cannot generate inputs for this promise". Keep the promise. Run the other examples, promises and tests by name with `--only`, as `--list` prints them.

For a false promise, `mo test` shrinks hidden inputs by deleting builder calls and simplifying their arguments, never by editing hidden fields. Same-call companions stay whole. Builder and structural shrinking share 200 additional subject trials and the original deadline; the result is not guaranteed minimal. Each generated tuple can record at most 4096 builder recipes, including recipes made during unsuccessful starts. If recording runs out, generation continues unchanged. A passing tuple still passes; a failing tuple keeps its original counterexample and reports E1404 with the headline "promise shrinking is limited because builder recording is incomplete".

For a promise on a generic function, `mo test` generates and shrinks inputs for every concrete instantiation already in the program, including anchors and program uses. It keeps complete type-argument lists, without cross-products or default types. `--list` and `--only` use the source promise's name. Each concrete instance has a separate result and counts separately in the totals. Its text result names the instance, and its JSON result adds an `instance` field with the concrete name and types. An unsupported instance refuses the selected set before initialization or tests run.

The `lib/` folder holds library modules written in Mo, and every `mo` command loads them with your program. `mo check lib/list.mo` and `mo test lib/list.mo` check and test one of them on its own. `lib/list.mo` has `List`'s `map`, `filter`, `filter_map`, `flat_map`, `reduce`, `find`, `any?`, `all?`, `count`, `sort_by`, `sort_by_desc`, `sort`, `min`, `max` and `group_by`, `lib/map.mo` has `Map.update`, `lib/option.mo` has `Option.or_fail` and `Option.map`, `lib/result.mo` has `Result.map` and `Result.map_error`, `lib/file-error.mo` has `FileError.to_text`, `lib/json.mo` is the `Json` module (`use Json`, then `Json.parse`, `to_text` and `pretty`), and `lib/files.mo` and `lib/read-files.mo` have `walk` for `Files` and `ReadFiles`. `lib/text.mo` has no functions, only tests of `Text`'s operations, written from reference section 14 and run by `mo test lib/text.mo`. `lib/set.mo` likewise tests `Set`'s operations, `List.to_set` and Set text forms with `mo test lib/set.mo`. `lib/unicode16.mo` is the `Unicode16` module. After `use Unicode16`, its built-in `graphemes`, `display_width`, `normalize_nfc`, `lowercase` and `uppercase` operations follow Unicode 16.0. `mo test lib/unicode16.mo` tests them. `lib/time.mo` has no functions, only tests of `Time`'s, `Duration`'s and `Clock.fixed`'s operations, run by `mo test lib/time.mo`. The rest of section 14 is still built in. Section 10.5 names what is always in scope.

After `use Unicode16`, `Unicode16.letter?(c: Char) : Bool` tests Unicode 16.0 General_Category L (`Lu`, `Ll`, `Lt`, `Lm`, `Lo`), and `Unicode16.digit?(c: Char) : Bool` tests `Nd` (decimal digits). Neither includes `Nl`, `No` or marks, normalizes, or uses locale tailoring. For example, `text.chars.filter(fn(c) Unicode16.letter?(c) or Unicode16.digit?(c) end)` keeps letters and decimal digits.

After `use Json` (or `use Shop.Account`), a bare `Json` (or `Account`) in a type is the module's own type of that name, as reference 10.3 says: write `fn f(value: Json)`. `Json.Json` means the same type.

## Write this program

Use this shape.

```mo
## Greets the name given on the command line, or the world.
module Greet

fn greeting(name: Option(Text)) : Text
  examples
    greeting(Some("Ada")) == "Hello, Ada!"
    greeting(None) == "Hello, world!"
  end

  "Hello, #{name or "world"}!"
end

fn main(args: Args, console: Console)
  console.print(greeting(args.list.first))
end

test "only the first argument is used"
  console = Console.fake()
  main(Args.of(["Ada", "Grace"]), console)
  assert console.printed == ["Hello, Ada!"]
end
```

The `##` comment documents the module, and the module line names `Greet`. Sections 1 and 2.2.

`greeting` types its parameter and its result. Inside a body, types are inferred from all their uses in it. The body's value is the value of its last line. Section 5.1.

`name` is an `Option`. `Some` and `None` are its cases, and `name or "world"` gives the value inside `Some`, or `"world"` for `None`. Sections 3 and 8.1.

`"Hello, #{name or "world"}!"` inserts the text form of that value. Section 3.4.

`main` lists the capabilities the program needs, here `Args` and `Console`. Sections 11.1 and 11.2.

`examples` sits at the top of the body, then a blank line, then the code. Sections 12 and 12.1.

The `test` uses `Console.fake()`, `Args.of`, and `console.printed`. Sections 12.3 and 12.4.

`args.list.first` calls `first` with no other arguments. This file has no `promise`. Sections 5.4 and 12.2.

## Follow these rules

Write these forms. The section number states the rule.

- Where another language writes `items[i]` or `map[key]`, write `items.get(i)` or `map.get(key)`. `get` returns an `Option`. Sections 3.7 and 3.8.
- Bind a name once with `name = value`. A name is visible from its binding to the end of its block. To assign it again, declare `var name = value` where it is first bound, even when the assignment is inside a loop or a branch. Sections 6.1 and 6.2.
- Join texts by inserting them into one text, as in `"#{name}: #{score}"`. `+` adds numbers and does not join texts. Section 3.4.
- A list's length is `list.size`. A text's is `text.char_count` or `text.byte_count`. There is no `len` or `length`. Sections 3.4 and 14.
- Add an element with `items = items.append(x)`, and declare `var items` when you assign that name again. Sections 3.7 and 6.2.
- Mo has no exceptions, and there is no "unwrap". Return `Error(...)`, pass it on with `try`, and handle it with `case`. Where a failure truly cannot happen, write `stop` and the reason. Sections 8.2, 8.3, and 8.4.
- Cover every possible value in a `case`. On a choice, a whole arm cannot be `_` or a name, so list the cases. Inside a pattern, `_` or a name may stand for anything. Section 7.2.
- Pass a capability in only as a parameter. Make a fake only in tests, examples, and promises. Sections 11 and 12.4.
- Declare a `struct` or an `enum`. Records have no methods. Put functions in that module. Sections 4 and 5.4.
- To state that a function holds for every input, write a `promise` with a sentence, a body, an `allows` line, and a `forbids` line. Section 12.2.
- When another module needs a name, list it with `expose` and bring the module in with `use`. Section 10.

The program `Shout` in section 1 has no `examples` and no `promise`.

Pass function arguments by position. Only records and cases are built with named fields. Section 5.2.

Read the section for the rule.

- Section 1 is already in the sample as a module and `main`, and indentation carries no meaning.
- Section 2 is files, comments, names, and lines. The sample's opening comment is section 2.2.
- Section 3 is values and types, including the sample's text and lists. Mo never converts between types silently.
- Section 4 is a `struct` or an `enum`.
- Section 5 is recursion, generic functions, and functions as values. There are no default or optional parameters.
- Section 6 is a name bound once, and `var` for a later assignment.
- Section 7 is `if`, `case`, `is`, and loops.
- Section 8 is `Option` and `Result`. The sample uses `Option` and `or`.
- Section 9 is the five abilities, `where`, and `to_text` on a type with `Text`.
- Section 10 is `expose`, `use`, and constants. The sample is one module.
- Section 11 is the capabilities besides `Args` and `Console`, and narrowing a capability.
- Section 12 is examples, promises, and tests. The sample has examples and a test, and no promise.
- Section 13 is commands and done, under the headings below.
- Section 14 is the catalog. Call library functions as that section lists them, through the type.

Section 15 names later work, and it changes no current rule.

The executable grammar in `grammar/mo.y` is the authority. Appendix B summarizes it.

## Run these commands

Each command is given the file of the program's main module. `mo check FILE` reports the program's errors, `mo run FILE [ARGS...]` runs it with the arguments after the file's name, and `mo test FILE` checks its examples, promises and tests. `mo emit FILE -o OUT` writes its bytecode. `mo format` and `mo fix` rewrite the files they are given.

Duration units accept singular and plural spellings, including `millisecond`.
`mo format` writes `1.minute` and `2.minutes` on direct integer literals, using
their numeric value even in hexadecimal or binary. It preserves other receivers
and qualified function values, such as `n.minute`, `(1).minutes`, and `Int.minute`,
because it does not infer types (reference 3.9).

`mo guide` needs no file. It prints this page. `mo guide --reference` prints the reference.

`mo` exits with status 0 when everything passed, 1 when the program has errors or an example, promise or test fails, and 2 for a mistake in the command itself. Once the program runs, `mo run` exits with the program's own status, or 70 after a bug. Section 11.2 states the program's own status. Section 13.2 states that bug.

Section 13.4 names the selection flags and the limits.

## Work in this loop

Follow these steps.

1. Write the program. When a function has `examples` or a `promise`, put each block at the top of the body, then a blank line, then the code. Add a `test` in the same file when you check a scenario. Section 12.
2. Run `mo format` on those files.
3. Run `mo check FILE`. It reports each error it finds, in source order. Fix every error it reports before you run `mo check` again, not only the first. Handle E1103 and E1150 as Know this build says.
4. Run `mo fix FILE` when `mo check` shows a fix. When an error shows more than one fix, choose one and make that edit yourself. Appendix A.
5. Run `mo test FILE` only once `mo check` reports no errors but those Know this build names. While the program has other errors, `mo test` runs nothing and prints the same errors `mo check` does. Run `mo test FILE` until the examples, promises, and tests pass.
6. Run `mo run FILE` only when the task needs a running program.

If a command exits 1 for a reason that Know this build names, follow its instructions above, and continue. Section 13.4 describes `--json`.

## Stop when the task is done

The task is done when the program is the v0.1 Mo the reference describes for what it computes, including the specs and tests that behavior needs, and you have run the loop steps this build implements.

On a complete toolchain, `mo format`, `mo check`, and `mo test` exit 0. Run `mo run` when the task is a running program.

On this build, accept a remaining diagnostic only when Know this build names it: E1103 or E1150 on a form the reference requires and offers no other way to write the same behaviour, or E1400 on a promise whose inputs `mo test` cannot build. Deleting a required spec, promise or capability to silence a diagnostic leaves the task undone. Deleting a promise because `mo test` returns E1400 leaves the task undone.

E1408 is also an expected build limitation when `mo run` targets a service entry module. Keep the service declaration and test its ordinary handlers. Passing those tests does not prove that the service runtime works.

Where the reference says nothing, do not add a rule, and do not fill that silence from the architecture or from `formal/TRANSLATION.md`.

Section 12 shows the form of a spec. When the task is a program, read section 11.2 for `main`'s result and the program's exit status.
